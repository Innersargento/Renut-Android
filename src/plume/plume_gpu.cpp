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

// Frames of silence the audio output played for want of one from the title
// (the SDL driver in rexglue).
extern "C" uint64_t renut_audio_silent_frames();

REXCVAR_DEFINE_BOOL(renut_plume_async_pipelines, true, "reNut/Plume",
                    "Compile new Plume graphics pipelines on workers; until one is ready its draw is drawn "
                    "with a sibling pipeline or skipped. Pipelines the driver already has are taken at the "
                    "draw. Draws that export to memory always wait for theirs.");
REXCVAR_DEFINE_INT32(renut_plume_pipeline_lookup_ms, 4, "reNut/Plume",
                     "Milliseconds per frame the command thread may spend asking the driver's cache for "
                     "new pipelines it already has, so their draws appear at once; past it they go to the "
                     "workers. 0 leaves every new pipeline to the workers.")
    .range(0, 33);
REXCVAR_DEFINE_INT32(renut_plume_bool_spec, 0, "reNut/Plume",
                     "How shaders see the Xenos boolean registers: 0 read from the shared block every draw "
                     "(about a quarter of the pipelines to compile), 1 as specialization constants (a "
                     "pipeline per combination of the bits a pair of shaders tests)")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_defer_draws, 2, "reNut/Plume",
                     "Record draws in windows grouped by pipeline: 0 off, 1 depth-only draws (the shadow "
                     "maps), 2 also opaque colour draws that test and write depth (reordering them is "
                     "exact except for depth ties; measured 11/10: -37% pipeline binds, -3% GPU cycles, "
                     "image unchanged)")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_hiz, 2, "reNut/Plume",
                     "Hi-Z culling: 0 off, 1 only counts what could be dropped, 2 drops the draws whose bounding box "
                     "lies wholly outside the view frustum or the scissor (exact; files/plume-hiz overrides)");
REXCVAR_DEFINE_BOOL(renut_plume_shader_cache, true, "reNut/Plume",
                    "Remember across frames which shader lives at a guest address and length, trusting it "
                    "until the guest writes there (write-watched), instead of hashing the microcode of "
                    "every shader a frame uses again (files/plume-shadercache overrides)");
REXCVAR_DEFINE_BOOL(renut_plume_key_reuse, true, "reNut/Plume",
                    "Key a draw's pipeline by one hash of its vertex shader's fetch table, kept on the "
                    "shader, instead of laying every specialization constant of the table into the key "
                    "each draw (files/plume-keyreuse overrides)");
REXCVAR_DEFINE_INT32(renut_plume_fetch_spec, 1, "reNut/Plume",
                     "Give each vertex shader's live vertex fetch table to the driver as specialization "
                     "constants, so that the format switches of its fetch sites fold away: 0 off, 1 on")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_depth_strip, 1, "reNut/Plume",
                     "Draw depth-only draws (no fragment shader: the shadow maps) with a vertex shader "
                     "that has lost its interpolant outputs, which nothing reads: 0 off, 1 on")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_rel_constants, 2, "reNut/Plume",
                     "How a vertex shader's relative constant reads (a0, aL: skinning matrices) are served: "
                     "0 by the file's four blocks, 1 by the whole file as one block, 2 by that block "
                     "declared too large for the driver to promote into the constant file")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_adaptive_patches, 1, "reNut/Plume",
                     "Tessellate adaptive triangle patches (the water) at the levels the game works out for "
                     "each edge, a patch off screen being one triangle, instead of every patch at the draw's "
                     "highest level: 0 off, 1 on")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(renut_plume_no_shadows, false, "reNut/Plume",
                    "Drop the shadow casters (depth-only draws into the shadow maps): the scene is drawn "
                    "without shadows.");
REXCVAR_DEFINE_BOOL(renut_plume_no_bloom, false, "reNut/Plume",
                    "Make the bloom's bright pass write black, so the blur chain and the final blend add "
                    "no glow.");
REXCVAR_DEFINE_BOOL(renut_plume_memexport_readback, false, "reNut/Plume",
                    "Copy shader exports back to guest memory (experimental; may overwrite reused memory).");
REXCVAR_DEFINE_BOOL(renut_plume_memexport_fast, true, "reNut/Plume",
                    "Batch Plume memexport readback across frames; off waits for the current frame.");
REXCVAR_DEFINE_INT32(renut_plume_low_memory, -1, "reNut/Plume",
                     "Keep less on the GPU for a device with little RAM: -1 decides from the device (under 6 GB), "
                     "0 off, 1 on.");
REXCVAR_DEFINE_INT32(renut_plume_texture_max_dim, 0, "reNut/Plume",
                     "Largest side a guest texture keeps on the host; the top mips of a larger one are dropped. "
                     "0 follows low-memory mode (512 there, no limit otherwise).");
REXCVAR_DEFINE_INT32(renut_plume_bc_textures, 1, "reNut/Plume",
                     "Upload the guest's DXT1/2/3/4/5, DXT5A and DXN 2D textures as the BC1-BC5 blocks they "
                     "already are (a quarter to an eighth of the RGBA8 the decoder makes, no CPU decode) on a GPU "
                     "that samples BC (Adreno): 0 off, 1 on. Live: files/plume-bc.")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_texture_quality, 0, "reNut/Plume",
                     "Texture quality: 0 full, 1 half, 2 quarter. A 2D texture the game samples with mip "
                     "filtering (the world; the interface and fonts are read from the base level alone and "
                     "stay sharp) drops its top one or two mips, never below 64 px. Live: "
                     "files/plume-texquality.")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_shadow_quality, 0, "reNut/Plume",
                     "Shadow map resolution: 0 full, 1 three quarters, 2 half (a side of the dynamic maps; "
                     "the cached atlas is not touched). The maps are drawn and resolved smaller and sampled "
                     "with the same coordinates. Read at start.")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_internal_resolution, 0, "reNut/Plume",
                     "Internal resolution of the scene: 0 full, 1 three quarters (960x540), 2 half (640x360) of "
                     "the guest's 1280x720. The scene's colour and depth surfaces are drawn smaller, resolved "
                     "smaller and shown stretched. Read at start.")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(renut_plume_precompile, true, "reNut/Plume",
                    "Record every pipeline the game uses (files/plume-recipes.bin) and build them all at the "
                    "next start; a scene load waits until they are built. Not in low-memory mode, which only "
                    "records.");

// game_fixes.cpp: how the MSAA-off path treats the scene's band split
// (<files>/plume-keep-split: 0 clear, 1 leave, 2 whole-screen projection).
extern std::atomic<int> g_renut_scene_split_mode;
REXCVAR_DECLARE(bool, renut_scene_msaa);
REXCVAR_DECLARE(bool, vsync);  // the SDK's: off ticks the guest's vblank every millisecond

namespace renut {
namespace {

using namespace plume;
namespace gfx = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
// Register indices (XE_GPU_REG_*).
using rex::graphics::Register;
using namespace rex::graphics;

#include "shaders/point_list.geom.inc"
#include "shaders/depth_stencil_pack.comp.inc"
#include "shaders/hiz_reduce.comp.inc"
#include "shaders/hiz_mips.comp.inc"
#include "shaders/hiz_test.comp.inc"
#include "shaders/patch_expand.comp.inc"
#include "shaders/patch_levels.comp.inc"
#include "shaders/rect_list.geom.inc"

// The push constants: the guest's three block addresses (24 bytes). The range is
// declared wider than that.
constexpr uint32_t kPushBytes = 128;

// Where microcode without a recompiled shader is saved (SetPlumeMissDumpDirectory).
std::string g_miss_dump_directory;
bool g_dedupe_descriptors = false;  // files/plume-dedupe-desc (see SetTexture2D)
bool g_validate_windows = false;    // files/plume-validate (see ValidateRange)

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
const UcodeMapEntry kUcodeMap[] = {
#include "shader_ucode_map.inc"
};

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

uint32_t BSwap32(uint32_t v) { return __builtin_bswap32(v); }

uint64_t ThreadCpuMicroseconds() {
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
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t size, xenos::Endian endian) {
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
std::vector<std::pair<uint32_t, bool>> ReflectInputs(const std::vector<uint32_t>& spirv) {
  std::unordered_map<uint32_t, uint32_t> location;   // id -> location
  std::unordered_map<uint32_t, uint32_t> variable;   // id -> pointer type (Input only)
  std::unordered_map<uint32_t, uint32_t> pointee;    // pointer type -> pointee
  std::unordered_map<uint32_t, uint32_t> component;  // vector type -> component type
  std::unordered_set<uint32_t> ints;
  for (size_t i = 5; i < spirv.size();) {
    uint32_t word = spirv[i], count = word >> 16, op = word & 0xFFFF;
    if (count == 0 || i + count > spirv.size()) break;
    const uint32_t* w = spirv.data() + i;
    if (op == 71 && count >= 4 && w[2] == 30) location[w[1]] = w[3];       // OpDecorate Location
    else if (op == 59 && count >= 4 && w[3] == 1) variable[w[2]] = w[1];   // OpVariable Input
    else if (op == 32 && count >= 4) pointee[w[1]] = w[3];                 // OpTypePointer
    else if (op == 23 && count >= 4) component[w[1]] = w[2];               // OpTypeVector
    else if (op == 21) ints.insert(w[1]);                                  // OpTypeInt
    i += count;
  }
  std::vector<std::pair<uint32_t, bool>> inputs;
  for (auto& [id, type] : variable) {
    auto loc = location.find(id);
    if (loc == location.end()) continue;  // built-ins
    uint32_t base = pointee.count(type) ? pointee[type] : 0;
    uint32_t scalar = component.count(base) ? component[base] : base;
    inputs.emplace_back(loc->second, ints.count(scalar) != 0);
  }
  return inputs;
}

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
    for (const auto& m : kUcodeMap) {
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
uint64_t RecipeProgram(const Recipe& r) {
  KeyBytes bytes;
  bytes.Put(r.vs);
  bytes.Put(r.ps);
  bytes.Put(r.domain);
  bytes.Put(r.form);
  bytes.Put(r.gs);
  uint64_t hash = bytes.Hash();
  for (const RenderSpecConstant& c : r.constants) {
    HashCombine(hash, c.index);
    HashCombine(hash, c.value);
  }
  return hash;
}

// A scene tag in the file: a record whose size has this bit set holds the
// recipe's key (64 bits) and the scene it was drawn in (32) instead of a recipe.
constexpr uint32_t kRecipeTagBit = 0x80000000u;
constexpr uint32_t kRecipeTagSize = 12;

uint64_t SceneTagKey(uint64_t recipe, uint32_t scene) {
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
  std::deque<uint32_t> urgent;
  size_t next = 0;
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
  // The scene the game asked for last (0 before the first: the boot); the
  // pipelines drawn are tagged with it.
  std::atomic<uint32_t> scene{0};

  // Under mutex. UINT32_MAX when every group is taken.
  uint32_t TakeGroup() {
    while (!urgent.empty()) {
      const uint32_t g = urgent.front();
      urgent.pop_front();
      if (state[g] == kPending) {
        state[g] = kTaken;
        return g;
      }
    }
    while (next < state.size()) {
      const uint32_t g = uint32_t(next++);
      if (state[g] == kPending) {
        state[g] = kTaken;
        return g;
      }
    }
    return UINT32_MAX;
  }
};
PrecompileState g_precompile;

// Decodes the vertex layout a bound vertex shader really fetches. Direct3D
// Which texture fetch constants a shader reads, as a bit per slot. The draw
// path used to ask every one of the 32 slots for its texture, sampler and
// resolved target, on every draw; a shader samples two or three. The constant a
// tfetch names sits in the same bits as a vfetch's, and Direct3D does not
// rewrite it.
uint32_t TextureMask(const gfx::Shader& shader) {
  const uint32_t* u = shader.ucode_dwords();
  const size_t count = shader.ucode_dword_count();
  std::vector<uint8_t> be(count * 4);
  for (size_t i = 0; i < count; ++i) ucode::StoreBE(be.data() + i * 4, u[i]);
  uint32_t mask = 0;
  for (uint32_t slot : ucode::FetchSlots(be.data(), be.size())) {
    if (size_t(slot) * 3 + 2 >= count) continue;
    const uint32_t w0 = u[size_t(slot) * 3];
    if ((w0 & 0x1F) == 0) continue;  // a vfetch, not a texture fetch
    mask |= 1u << ((w0 >> 20) & 0x1F);
  }
  return mask;
}

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
uint32_t FetchRemap(uint32_t compiled, uint32_t live) {
  if (compiled == live) return 0;
  uint32_t remap = RENUT_INPUT_REMAP_IDENTITY;
  uint32_t set = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t c = (compiled >> (3 * i)) & 7, l = (live >> (3 * i)) & 7;
    if (c >= 4 || l == 7) {
      // A lane the recompiled code fills with a constant or leaves alone, or
      // one the live code leaves alone: nothing to take from memory.
      if (c != l && c != 7 && l != 7) return ~0u;
      continue;
    }
    if (l == 6) return ~0u;
    const uint32_t shift = 3 * c;
    if ((set >> c) & 1) {
      if (((remap >> shift) & 7) != l) return ~0u;
      continue;
    }
    set |= 1u << c;
    remap = (remap & ~(7u << shift)) | (l << shift);
  }
  return remap ^ RENUT_INPUT_REMAP_IDENTITY;
}

void DecodeLiveElements(HostShader& shader, const ShaderCacheEntry* entry,
                        std::vector<LiveElement>& out_elements,
                        std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>>& out_fetches) {
  const uint32_t* u = shader.ucode_dwords();
  const size_t slots = shader.ucode_dword_count() / 3;
  // Fetch slots in program order, to find the full fetch a mini fetch follows.
  std::vector<uint8_t> be(shader.ucode_dword_count() * 4);
  for (size_t i = 0; i < shader.ucode_dword_count(); ++i) ucode::StoreBE(be.data() + i * 4, u[i]);
  const std::vector<uint32_t> fetch_slots = ucode::FetchSlots(be.data(), be.size());
  const ShaderInputElement* elements = g_shaderInputElements + entry->inputElementOffset;
  for (uint32_t i = 0; i < entry->inputElementCount; ++i) {
    const ShaderInputElement& e = elements[i];
    if (e.location == 0xFF || e.fetchAddress >= slots) continue;
    const bool live_fetch = e.location == RENUT_LIVE_FETCH_LOCATION;
    const uint32_t* f = u + size_t(e.fetchAddress) * 3;
    if ((f[0] & 0x1F) != 0) continue;
    const uint32_t* full = f;
    if ((f[1] >> 30) & 1) {  // mini fetch: stream and stride from the preceding full fetch
      full = nullptr;
      for (uint32_t slot : fetch_slots) {
        if (slot >= e.fetchAddress) break;
        const uint32_t* g = u + size_t(slot) * 3;
        if ((g[0] & 0x1F) == 0 && !((g[1] >> 30) & 1)) full = g;
      }
      if (!full) continue;
    }
    const uint32_t fetch_constant = ((full[0] >> 20) & 0x1F) * 3 + ((full[0] >> 25) & 0x3);
    if (fetch_constant > 95 || 95 - fetch_constant >= 16) continue;
    uint32_t remap = FetchRemap(e.dstSwizzle, f[1] & 0xFFF);
    if (remap == ~0u) {
      remap = 0;
      static uint32_t logged = 0;
      if (logged < 32) {
        ++logged;
        REXLOG_WARN("Renut: Plume vs {:016X} fetch @{} reads swizzle {:03X}, recompiled with {:03X}",
                    shader.ucode_data_hash(), e.fetchAddress, f[1] & 0xFFF, uint32_t(e.dstSwizzle));
      }
    }
    if (live_fetch) {
      if (e.slot >= RENUT_LIVE_FETCH_COUNT) continue;
      const uint32_t flags = ((f[1] >> 12) & 1) | (((f[1] >> 13) & 1) << 1) | (((f[1] >> 14) & 1) << 2);
      const int32_t exp_adjust = int32_t(f[1] << 2) >> 26;  // bits 24..29, signed
      const float scale = std::ldexp(1.f, exp_adjust);
      uint32_t scale_bits;
      std::memcpy(&scale_bits, &scale, 4);
      out_fetches.push_back(
          {e.slot, {(95 - fetch_constant) | (((f[1] >> 16) & 0x3F) << 8) | (flags << 16),
                    (full[2] & 0xFF) * 4 | (remap << 16), ((f[2] >> 8) & 0x7FFFFF) * 4, scale_bits}});
      continue;
    }
    // The input assembler has no exponent bias; only vfetchLive applies one.
    if (const int32_t exp_adjust = int32_t(f[1] << 2) >> 26) {
      static uint32_t logged = 0;
      if (logged < 32) {
        ++logged;
        REXLOG_WARN("Renut: Plume vs {:016X} fetch @{} scales by 2^{}, which its attribute ignores",
                    shader.ucode_data_hash(), e.fetchAddress, exp_adjust);
      }
    }
    LiveElement live;
    live.location = e.location;
    live.slot = 95 - fetch_constant;
    live.stride = (full[2] & 0xFF) * 4;
    live.offset = ((f[2] >> 8) & 0x7FFFFF) * 4;
    live.format = (f[1] >> 16) & 0x3F;
    live.is_signed = (f[1] >> 12) & 1;
    live.normalized = !((f[1] >> 13) & 1);
    live.usage = e.usage;
    live.usage_index = e.usageIndex;
    live.remap = remap;
    out_elements.push_back(live);
  }
}

void DecodeLiveElements(HostShader& shader) {
  shader.elements_decoded = true;
  DecodeLiveElements(shader, shader.entry, shader.elements, shader.live_fetches);
}

// ---------------------------------------------------------------------------
// Format and state translation
// ---------------------------------------------------------------------------

// A pipeline the driver would not build: Plume still hands back the object, with
// a null Vulkan handle, and binding that hangs some drivers. The callers tell
// failure from success by the pointer.
std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(RenderDevice* device, const RenderGraphicsPipelineDesc& desc) {
  auto pipeline = device->createGraphicsPipeline(desc);
  if (pipeline && static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk == VK_NULL_HANDLE) return nullptr;
  return pipeline;
}

// A texture's subresource into a readback buffer, `row_texels` texels to a row.
// Plume's copyTextureRegion goes the other way only, so this is Vulkan on the
// command buffer itself; the texture is in COPY_SOURCE (transfer source).
void CopyTextureToBuffer(RenderCommandList* list, RenderBuffer* buffer, RenderTexture* texture, uint32_t width,
                         uint32_t height, uint32_t row_texels, uint32_t mip, uint32_t layer, const RenderBox* box) {
  VkBufferImageCopy copy{};
  copy.bufferRowLength = row_texels;
  copy.bufferImageHeight = height;
  const bool depth = static_cast<VulkanTexture*>(texture)->desc.flags & RenderTextureFlag::DEPTH_TARGET;
  copy.imageSubresource.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.mipLevel = mip;
  copy.imageSubresource.baseArrayLayer = layer;
  copy.imageSubresource.layerCount = 1;
  if (box) {
    copy.imageOffset.x = box->left;
    copy.imageOffset.y = box->top;
  }
  copy.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(list)->vk, static_cast<VulkanTexture*>(texture)->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(buffer)->vk, 1, &copy);
}

RenderFormat ColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return RenderFormat::R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return RenderFormat::R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return RenderFormat::R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return RenderFormat::R32G32_FLOAT;
    default:
      // 2_10_10_10 (fixed and float) and 16_16_16_16 (fixed and float).
      return RenderFormat::R16G16B16A16_FLOAT;
  }
}

bool IsBlendable(RenderFormat format) {
  return format != RenderFormat::R32_FLOAT && format != RenderFormat::R32G32_FLOAT;
}

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

RenderFormat VertexFormat(uint32_t format, bool is_signed, bool normalized, InputDecode& decode) {
  decode = InputDecode::kNone;
  // The shaders take every input as a float4 and, for the locations flagged
  // integer, read the bits of a UINT/SINT attribute with asint(). That works
  // where such an attribute reaches a float input unconverted (Adreno) and not
  // where the hardware converts it (Mali): the bone index of a skinned mesh
  // came out as asint(28.0) and the vertex shader addressed its matrix palette
  // gigabytes away - a page fault that took the whole device down. A UBYTE4 or
  // SHORT is a number to the guest anyway, so for the 8 and 16-bit formats the
  // hardware does the conversion (USCALED/SSCALED) and nothing is flagged.
  auto integer = [&](RenderFormat unsigned_format, RenderFormat signed_format) {
    decode = InputDecode::kInteger;
    return is_signed ? signed_format : unsigned_format;
  };
  auto scaled = [&](RenderFormat unsigned_format, RenderFormat signed_format) {
    return is_signed ? signed_format : unsigned_format;
  };
  switch (format) {
    case 6:
      return normalized ? (is_signed ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM)
                        : scaled(RenderFormat::R8G8B8A8_UINT, RenderFormat::R8G8B8A8_SINT);
    case 25:
      return normalized ? (is_signed ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM)
                        : scaled(RenderFormat::R16G16_UINT, RenderFormat::R16G16_SINT);
    case 26:
      return normalized
                 ? (is_signed ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM)
                 : scaled(RenderFormat::R16G16B16A16_UINT, RenderFormat::R16G16B16A16_SINT);
    case 31: return RenderFormat::R16G16_FLOAT;
    case 32: return RenderFormat::R16G16B16A16_FLOAT;
    case 33: return integer(RenderFormat::R32_UINT, RenderFormat::R32_SINT);
    case 34: return integer(RenderFormat::R32G32_UINT, RenderFormat::R32G32_SINT);
    case 35: return integer(RenderFormat::R32G32B32A32_UINT, RenderFormat::R32G32B32A32_SINT);
    case 36: return RenderFormat::R32_FLOAT;
    case 37: return RenderFormat::R32G32_FLOAT;
    case 38: return RenderFormat::R32G32B32A32_FLOAT;
    case 57: return RenderFormat::R32G32B32_FLOAT;
    case 7:
      // unpackInput decodes only the signed normalized form (normals and
      // tangents). An unsigned one read that way turned its 2-bit W of 1 into
      // -1 and its halves above 0.5 negative: the pieces of Banjo that fly
      // into a world portal (35D0D3DF02172D01) streaked across the screen.
      if (!is_signed) {
        return normalized ? kVertexUnorm1010102 : kVertexUscaled1010102;
      }
      decode = InputDecode::kPacked1010102;
      return RenderFormat::R32_FLOAT;
    case 16:
    case 17:
      decode = InputDecode::kPacked111110;
      return RenderFormat::R32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

bool IsSixteenBitFormat(uint32_t format) {
  return format == 25 || format == 26 || format == 31 || format == 32;
}

// Xenos k_11_11_10 (vertex format 17) keeps X in the low 10 bits and Y and Z
// in 11 bits each; k_10_11_11 (16) is the other way round (X and Y 11 bits, Z
// 10). tfetchR11G11B10 in the recompiled shaders decodes only the second
// layout, so a format 17 vector - the ground decals' tangent and binormal -
// came out a different, non-unit vector for every small change of its bits,
// and the tread ribbon's relief and light jumped from quad to quad. Move the
// same vector into the layout the shader reads.
uint32_t RepackFormat17(uint32_t word) {
  auto signed_field = [](uint32_t value, int bits) { return int32_t(value << (32 - bits)) >> (32 - bits); };
  const float x = std::max(-1.0f, float(signed_field(word & 0x3FF, 10)) / 511.0f);
  const float y = std::max(-1.0f, float(signed_field((word >> 10) & 0x7FF, 11)) / 1023.0f);
  const float z = std::max(-1.0f, float(signed_field(word >> 21, 11)) / 1023.0f);
  auto quantize = [](float v, float scale, int32_t lo, int32_t hi) {
    return std::clamp(int32_t(std::lround(v * scale)), lo, hi);
  };
  const int32_t xi = quantize(x, 1024.0f, -1024, 1023);
  const int32_t yi = quantize(y, 1024.0f, -1024, 1023);
  const int32_t zi = quantize(z, 512.0f, -512, 511);
  return (uint32_t(xi) & 0x7FF) | ((uint32_t(yi) & 0x7FF) << 11) | ((uint32_t(zi) & 0x3FF) << 22);
}

RenderBlend Blend(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero: return RenderBlend::ZERO;
    case xenos::BlendFactor::kOne: return RenderBlend::ONE;
    case xenos::BlendFactor::kSrcColor: return RenderBlend::SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return RenderBlend::INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return RenderBlend::SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return RenderBlend::INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return RenderBlend::DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return RenderBlend::INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return RenderBlend::DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return RenderBlend::INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha: return RenderBlend::BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha: return RenderBlend::INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
  }
}

RenderBlendOperation BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract: return RenderBlendOperation::SUBTRACT;
    case xenos::BlendOp::kMin: return RenderBlendOperation::MIN;
    case xenos::BlendOp::kMax: return RenderBlendOperation::MAX;
    case xenos::BlendOp::kRevSubtract: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}

RenderComparisonFunction Compare(xenos::CompareFunction f) {
  switch (f) {
    case xenos::CompareFunction::kNever: return RenderComparisonFunction::NEVER;
    case xenos::CompareFunction::kLess: return RenderComparisonFunction::LESS;
    case xenos::CompareFunction::kEqual: return RenderComparisonFunction::EQUAL;
    case xenos::CompareFunction::kLessEqual: return RenderComparisonFunction::LESS_EQUAL;
    case xenos::CompareFunction::kGreater: return RenderComparisonFunction::GREATER;
    case xenos::CompareFunction::kNotEqual: return RenderComparisonFunction::NOT_EQUAL;
    case xenos::CompareFunction::kGreaterEqual: return RenderComparisonFunction::GREATER_EQUAL;
    default: return RenderComparisonFunction::ALWAYS;
  }
}

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
uint64_t HostTextureBytes(const GuestTexture& t) {
  const uint64_t base = t.bc != BcKind::kNone
                            ? uint64_t((t.width + 3) / 4) * ((t.height + 3) / 4) * BcBlockBytes(t.bc)
                            : uint64_t(t.width) * t.height * 4;
  return t.levels > 1 ? base * 4 / 3 : base;
}

uint64_t ContentHash(const uint8_t* data, uint32_t span);

// Bytes per element of a memory export format, 0 for the ones renutMemExport
// does not store (narrower than a dword).
uint32_t ExportElementBytes(uint32_t format) {
  switch (format) {
    case 6: case 14: case 50:  // 8_8_8_8
    case 7: case 54:           // 2_10_10_10
    case 16: case 55:          // 10_11_11
    case 17: case 56:          // 11_11_10
    case 25:                   // 16_16
    case 31:                   // 16_16_FLOAT
    case 36:                   // 32_FLOAT
      return 4;
    case 26:  // 16_16_16_16
    case 32:  // 16_16_16_16_FLOAT
    case 37:  // 32_32_FLOAT
      return 8;
    case 38:  // 32_32_32_32_FLOAT
      return 16;
    default:
      return 0;
  }
}

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
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
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
  // Resolves come as records (NativeResolve); the ring's copy draws do nothing.
  bool IssueCopy() { return true; }
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
  static constexpr uint32_t kHizMinWindow = 12;
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

uint64_t PlumeCommandProcessor::ProgramKey(const RenderGraphicsPipelineDesc& desc) {
  KeyBytes bytes;
  bytes.Put(desc.vertexShader);
  bytes.Put(desc.geometryShader);
  bytes.Put(desc.pixelShader);
  for (uint32_t i = 0; i < desc.specConstantsCount; ++i) {
    bytes.Put(desc.specConstants[i].index);
    bytes.Put(desc.specConstants[i].value);
  }
  return bytes.Hash();
}

void PlumeCommandProcessor::StartPipelineWorker() {
  if (!pipeline_workers_.empty()) return;
  stop_pipeline_worker_ = false;
  lookup_on_workers_ = PlumePipelineLookupSupported();
  // Compiling is slow on one core and free on others: a few workers, kept off
  // the cores' best share so the guest and command list threads win contention.
  const uint32_t cores = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t count = std::clamp(cores / 3, 1u, 3u);
  // One more on a device with cores to spare, for work whose draw is missing
  // from the screen only: it sleeps whenever nothing is.
  const uint32_t urgent_only = cores >= 8 ? 1 : 0;
  for (uint32_t i = 0; i < count + urgent_only; ++i) {
    pipeline_workers_.emplace_back([this, i, urgent = i >= count] {
      pthread_setname_np(pthread_self(), fmt::format("PlumePipeline{}", i).c_str());
      int nice = INT32_MIN;
      for (;;) {
        std::shared_ptr<PipelineJob> job;
        {
          std::unique_lock lock(pipeline_mutex_);
          pipeline_ready_.wait(lock, [this, urgent] {
            return stop_pipeline_worker_ || !pipeline_lookup_queue_.empty() || !pipeline_urgent_queue_.empty() ||
                   (!urgent && !pipeline_queue_.empty());
          });
          if (stop_pipeline_worker_) return;
          // Lookups and the rest oldest first, so a steady stream of new
          // pipelines cannot starve one that was queued earlier; urgent ones
          // by how much their draw is missed.
          if (!pipeline_lookup_queue_.empty()) {
            job = std::move(pipeline_lookup_queue_.front());
            pipeline_lookup_queue_.pop_front();
          } else if (!pipeline_urgent_queue_.empty()) {
            job = TakeUrgentJob();
          } else {
            job = std::move(pipeline_queue_.front());
            pipeline_queue_.pop_front();
          }
        }
        // A job whose draw is missing from the screen competes for the CPU; one
        // that only replaces a stand-in yields to the game further.
        const int wanted = job->urgent ? 4 : 10;
        if (wanted != nice) {
          renutSetThreadNice(wanted);
          nice = wanted;
        }
        job->desc.inputElements = job->elements.data();
        job->desc.inputSlots = job->slots.data();
        job->desc.specConstants = job->constants.data();
        const auto start = std::chrono::steady_clock::now();
        auto finish = [&] {
          job->duration_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start).count());
          job->done.store(true, std::memory_order_release);
        };
        // First the cache: a sibling of the same program may have compiled it
        // since the job was queued. A miss joins the compiles, behind the
        // lookups still waiting.
        if (lookup_on_workers_ && !job->looked_up) {
          {
            PlumePipelineLookupOnly only;
            try { job->result = CreateGraphicsPipeline(device_, job->desc); } catch (const std::exception&) {}
          }
          job->looked_up = true;
          if (job->result) {
            job->found = true;
            finish();
          } else {
            {
              std::lock_guard lock(pipeline_mutex_);
              (job->urgent ? pipeline_urgent_queue_ : pipeline_queue_).push_back(std::move(job));
            }
            pipeline_ready_.notify_all();
          }
          continue;
        }
        // Then one compile per program: a job whose program another worker is
        // compiling waits for that compile and looks it up afterwards.
        {
          std::lock_guard lock(pipeline_mutex_);
          auto [waiting, first] = compiling_programs_.try_emplace(job->program);
          if (!first) {
            job->waited = true;
            waiting->second.push_back(std::move(job));
            continue;
          }
        }
        try { job->result = CreateGraphicsPipeline(device_, job->desc); }
        catch (const std::exception& e) { REXLOG_WARN("Renut: Plume async pipeline failed: {}", e.what()); }
        const uint64_t program = job->program;
        finish();
        bool requeued = false;
        {
          std::lock_guard lock(pipeline_mutex_);
          auto waiting = compiling_programs_.find(program);
          if (waiting != compiling_programs_.end()) {
            for (auto& sibling : waiting->second) {
              sibling->looked_up = false;
              (lookup_on_workers_ ? pipeline_lookup_queue_
               : sibling->urgent  ? pipeline_urgent_queue_
                                  : pipeline_queue_).push_front(std::move(sibling));
              requeued = true;
            }
            compiling_programs_.erase(waiting);
          }
        }
        if (requeued) pipeline_ready_.notify_all();
      }
    });
  }
}

void PlumeCommandProcessor::StopPipelineWorker() {
  if (pipeline_workers_.empty()) return;
  {
    std::lock_guard lock(pipeline_mutex_);
    stop_pipeline_worker_ = true;
    pipeline_lookup_queue_.clear();
    pipeline_urgent_queue_.clear();
    pipeline_queue_.clear();
    compiling_programs_.clear();
  }
  pipeline_ready_.notify_all();
  for (std::thread& worker : pipeline_workers_) worker.join();
  pipeline_workers_.clear();
  pending_pipelines_.clear();
  pending_programs_.clear();
}

void PlumeCommandProcessor::CountJob(const PipelineJob& job) {
  stats_.pipeline_us += job.duration_us;
  ++stats_.pipelines;
  if (job.found) {
    ++stats_.pipelines_found_ahead;
  } else {
    ++stats_.pipelines_compiled;
    stats_.compile_us += job.duration_us;
  }
  if (job.waited) ++stats_.pipelines_waited;
  auto program = pending_programs_.find(job.program);
  if (program != pending_programs_.end() && --program->second == 0) pending_programs_.erase(program);
}

// A finished job used to wait for its own draw to come back and claim it. One
// that never did (a scene change, a one-off effect) kept its slot forever, and
// once 128 of those piled up every new pipeline compiled on the command list
// thread again. Called once a frame, this installs them all.
void PlumeCommandProcessor::ReapPipelines() {
  for (auto it = pending_pipelines_.begin(); it != pending_pipelines_.end();) {
    PipelineJob& job = *it->second;
    if (!job.done.load(std::memory_order_acquire)) {
      ++it;
      continue;
    }
    CountJob(job);
    if (job.result) {
      std::unique_ptr<RenderPipeline>& slot = pipelines_[it->first];
      slot = std::move(job.result);
      standins_[job.family] = slot.get();
    } else {
      failed_pipelines_.insert(it->first);
      REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})", job.vs_hash, job.ps_hash);
    }
    it = pending_pipelines_.erase(it);
  }
}

void PlumeCommandProcessor::StartRecipes() {
  recipes_ = REXCVAR_GET(renut_plume_precompile) && !g_miss_dump_directory.empty();
  if (!recipes_) return;
  const std::string files = g_miss_dump_directory.substr(0, g_miss_dump_directory.rfind('/'));
  const std::string path = files + "/plume-recipes.bin";
  auto read_file = [](const std::string& name) {
    std::vector<uint8_t> bytes;
    if (FILE* file = std::fopen(name.c_str(), "rb")) {
      std::fseek(file, 0, SEEK_END);
      const long size = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      if (size > 0) {
        bytes.resize(size_t(size));
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
      }
      std::fclose(file);
    }
    return bytes;
  };
  // Records are a 32-bit size and a recipe's bytes, or a scene tag (the size
  // with kRecipeTagBit). One cut short by the app being killed mid-write ends
  // the list and is cut off the file.
  auto recipes = std::make_shared<std::vector<std::pair<uint64_t, Recipe>>>();
  std::vector<std::pair<uint64_t, uint32_t>> tags;
  RecipeHeader header;
  // Returns where the good records end, 0 for a file that is not a list.
  auto parse = [&](const std::vector<uint8_t>& bytes) -> size_t {
    if (bytes.size() < sizeof(header) || std::memcmp(bytes.data(), &header, sizeof(header)) != 0) return 0;
    size_t good_end = sizeof(header);
    while (bytes.size() - good_end >= sizeof(uint32_t)) {
      uint32_t size = 0;
      std::memcpy(&size, bytes.data() + good_end, sizeof(size));
      const uint8_t* at = bytes.data() + good_end + sizeof(size);
      const uint32_t length = size & ~kRecipeTagBit;
      if (size_t(bytes.data() + bytes.size() - at) < length) break;
      if (size & kRecipeTagBit) {
        if (length != kRecipeTagSize) break;
        uint64_t recipe = 0;
        uint32_t scene = 0;
        std::memcpy(&recipe, at, sizeof(recipe));
        std::memcpy(&scene, at + sizeof(recipe), sizeof(scene));
        if (scene_tags_.insert(SceneTagKey(recipe, scene)).second) tags.emplace_back(recipe, scene);
      } else {
        Recipe recipe;
        RecipeReader reader{at, at + length};
        const uint64_t key = XXH3_64bits(at, length);
        if (VisitRecipe(reader, recipe) && recorded_recipes_.insert(key).second) {
          recipes->emplace_back(key, std::move(recipe));
        }
      }
      good_end += sizeof(size) + length;
    }
    return good_end;
  };
  // The list shipped with the app (the app copies it out of the APK) comes
  // first: it stands in for the pipelines UnleashedRecomp predicts from the
  // assets a scene loads, so even the first start holds each loading screen
  // for its scene's pipelines instead of compiling them in play. The player's
  // own file only adds what the shipped list lacks.
  const size_t seeded = parse(read_file(files + "/plume-recipes.seed")) ? recipes->size() : 0;
  const std::vector<uint8_t> bytes = read_file(path);
  const size_t good_end = parse(bytes);
  const bool valid = good_end != 0;
  if (seeded) REXLOG_INFO("Renut: Plume pipeline recipes: {} shipped, {} more recorded here", seeded,
                          recipes->size() - seeded);
  if (!valid) {
    if ((recipe_file_ = std::fopen(path.c_str(), "wb"))) {
      std::fwrite(&header, sizeof(header), 1, recipe_file_);
      std::fflush(recipe_file_);
    }
  } else {
    if (good_end != bytes.size()) truncate(path.c_str(), off_t(good_end));
    recipe_file_ = std::fopen(path.c_str(), "ab");
  }
  if (!recipe_file_) REXLOG_WARN("Renut: Plume pipeline recipes cannot be written to {}", path);
  if (recipes->empty() || low_memory_) {
    REXLOG_INFO("Renut: Plume pipeline recipes: {} in {}{}", recipes->size(), path,
                recipes->empty() ? "" : "; not built ahead in low-memory mode");
    return;
  }

  // One group per program, built by one thread: its first recipe compiles
  // and the others find the program in the cache.
  auto groups = std::make_shared<std::vector<std::vector<uint32_t>>>();
  std::unordered_map<uint64_t, uint32_t> group_of_recipe;
  {
    std::unordered_map<uint64_t, uint32_t> group_of;
    for (uint32_t i = 0; i < recipes->size(); ++i) {
      auto [it, added] = group_of.try_emplace(RecipeProgram((*recipes)[i].second), uint32_t(groups->size()));
      if (added) groups->emplace_back();
      (*groups)[it->second].push_back(i);
      group_of_recipe.emplace((*recipes)[i].first, it->second);
    }
  }
  {
    std::lock_guard lock(g_precompile.mutex);
    PrecompileState& p = g_precompile;
    p.state.assign(groups->size(), PrecompileState::kPending);
    p.group_size.clear();
    for (const auto& group : *groups) p.group_size.push_back(uint32_t(group.size()));
    p.urgent.clear();
    p.next = 0;
    p.scene_groups.clear();
    for (const auto& [recipe, scene] : tags) {
      auto group = group_of_recipe.find(recipe);
      if (group == group_of_recipe.end()) continue;
      std::vector<uint32_t>& list = p.scene_groups[scene];
      if (std::find(list.begin(), list.end(), group->second) == list.end()) list.push_back(group->second);
    }
    // The boot's own first; then the rest in the order they were first drawn.
    if (auto boot = p.scene_groups.find(0); boot != p.scene_groups.end()) {
      p.urgent.assign(boot->second.begin(), boot->second.end());
    }
    // Running before the total: the app stops watching once it sees a total
    // with nothing running.
    p.running = true;
    p.done = 0;
    p.total = uint32_t(recipes->size());
  }
  stop_precompile_ = false;
  const uint32_t cores = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t count = std::clamp(cores * 2 / 3, 2u, 6u);
  REXLOG_INFO("Renut: Plume building {} pipelines ({} programs, {} tags over {} scenes) from {} on {} threads",
              recipes->size(), groups->size(), tags.size(), g_precompile.scene_groups.size(), path, count);
  auto left = std::make_shared<std::atomic<uint32_t>>(count);
  auto failed = std::make_shared<std::atomic<uint32_t>>(0);
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t t = 0; t < count; ++t) {
    precompile_threads_.emplace_back([=, this] {
      pthread_setname_np(pthread_self(), fmt::format("PlumePrebuild{}", t).c_str());
      int nice = INT32_MIN;
      for (;;) {
        if (stop_precompile_.load(std::memory_order_relaxed)) break;
        uint32_t g;
        {
          std::lock_guard lock(g_precompile.mutex);
          g = g_precompile.TakeGroup();
        }
        if (g == UINT32_MAX) break;
        // Behind the game while it plays; at full speed while its loading
        // screen is up.
        const int wanted = g_precompile.loading.load(std::memory_order_relaxed) ? 0 : 10;
        if (wanted != nice) {
          renutSetThreadNice(wanted);
          nice = wanted;
        }
        const std::vector<uint32_t>& group = (*groups)[g];
        const Recipe& first = (*recipes)[group[0]].second;
        CompiledShader vs, ps;
        const ShaderCacheEntry* vs_entry = store_->FindContainer(first.vs);
        const ShaderCacheEntry* ps_entry = first.ps ? store_->FindContainer(first.ps) : nullptr;
        bool ok = vs_entry && ShaderStore::BuildModules(device_, vs_entry, !first.domain, true, vs) &&
                  (!first.ps || (ps_entry && ShaderStore::BuildModules(device_, ps_entry, false, false, ps)));
        const RenderShader* vs_module = !ok                                   ? nullptr
                                        : first.form == kRecipeFetchSpec      ? vs.fetch_spec_module.get()
                                        : first.form == kRecipeDepth          ? vs.depth_module.get()
                                        : first.form == kRecipeDepthFetchSpec ? vs.depth_fetch_spec_module.get()
                                                                              : vs.module.get();
        for (const uint32_t index : group) {
          auto& [key, recipe] = (*recipes)[index];
          std::unique_ptr<RenderPipeline> pipeline;
          if (vs_module && !stop_precompile_.load(std::memory_order_relaxed)) {
            RenderGraphicsPipelineDesc desc = recipe.desc;
            desc.pipelineLayout = layout_.get();
            desc.vertexShader = vs_module;
            desc.geometryShader = recipe.gs == 1 ? rect_list_gs_.get() : recipe.gs == 2 ? point_list_gs_.get() : nullptr;
            desc.pixelShader = recipe.ps ? ps.module.get() : nullptr;
            desc.inputElements = recipe.elements.data();
            desc.inputElementsCount = uint32_t(recipe.elements.size());
            desc.inputSlots = recipe.slots.data();
            desc.inputSlotsCount = uint32_t(recipe.slots.size());
            desc.specConstants = recipe.constants.data();
            desc.specConstantsCount = uint32_t(recipe.constants.size());
            try { pipeline = CreateGraphicsPipeline(device_, desc); } catch (const std::exception&) {}
          }
          if (pipeline) {
            std::lock_guard lock(prebuilt_mutex_);
            prebuilt_.emplace(key, std::move(pipeline));
          } else {
            failed->fetch_add(1, std::memory_order_relaxed);
          }
        }
        {
          std::lock_guard lock(g_precompile.mutex);
          g_precompile.state[g] = PrecompileState::kDone;
        }
        g_precompile.done.fetch_add(uint32_t(group.size()), std::memory_order_relaxed);
        g_precompile.changed.notify_all();
      }
      if (left->fetch_sub(1) == 1) {
        REXLOG_INFO("Renut: Plume built {} of {} pipelines ahead in {} ms{}", g_precompile.done.load() - failed->load(),
                    g_precompile.total.load(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
                    stop_precompile_.load() ? " (stopped)" : "");
        {
          std::lock_guard lock(g_precompile.mutex);
          g_precompile.running = false;
        }
        g_precompile.changed.notify_all();
      }
    });
  }
}

void PlumeCommandProcessor::StopRecipes() {
  stop_precompile_ = true;
  for (std::thread& thread : precompile_threads_) thread.join();
  precompile_threads_.clear();
  {
    std::lock_guard lock(g_precompile.mutex);
    g_precompile.running = false;
  }
  g_precompile.changed.notify_all();
  {
    std::lock_guard lock(prebuilt_mutex_);
    prebuilt_.clear();
  }
  recorded_recipes_.clear();
  scene_tags_.clear();
  recipe_of_.clear();
  if (recipe_file_) std::fclose(recipe_file_);
  recipe_file_ = nullptr;
}

// Flushed at once: a game closed by swiping it away never runs the shutdown
// that would close the file.
void PlumeCommandProcessor::WriteRecord(uint32_t size, const void* data) {
  if (!recipe_file_) return;
  std::fwrite(&size, sizeof(size), 1, recipe_file_);
  std::fwrite(data, 1, size & ~kRecipeTagBit, recipe_file_);
  std::fflush(recipe_file_);
}

std::unique_ptr<RenderPipeline> PlumeCommandProcessor::TakeRecipe(Recipe& recipe, uint64_t key) {
  std::string bytes;
  RecipeWriter writer{bytes};
  VisitRecipe(writer, recipe);
  const uint64_t recipe_key = XXH3_64bits(bytes.data(), bytes.size());
  recipe_of_[key] = recipe_key;
  {
    std::lock_guard lock(prebuilt_mutex_);
    auto found = prebuilt_.find(recipe_key);
    if (found != prebuilt_.end()) {
      std::unique_ptr<RenderPipeline> pipeline = std::move(found->second);
      prebuilt_.erase(found);
      return pipeline;
    }
  }
  if (recorded_recipes_.insert(recipe_key).second) WriteRecord(uint32_t(bytes.size()), bytes.data());
  return nullptr;
}

void PlumeCommandProcessor::NoteSceneUse(uint64_t key) {
  const uint32_t scene = g_precompile.scene.load(std::memory_order_relaxed);
  if (scene != tag_scene_) {
    tag_scene_ = scene;
    scene_seen_.reset();
  }
  const size_t bit = size_t(key) & (scene_seen_.size() - 1);
  if (scene_seen_.test(bit)) return;
  scene_seen_.set(bit);
  auto recipe = recipe_of_.find(key);
  if (recipe == recipe_of_.end()) return;
  if (!scene_tags_.insert(SceneTagKey(recipe->second, scene)).second) return;
  uint8_t tag[kRecipeTagSize];
  std::memcpy(tag, &recipe->second, sizeof(uint64_t));
  std::memcpy(tag + sizeof(uint64_t), &scene, sizeof(scene));
  WriteRecord(kRecipeTagBit | kRecipeTagSize, tag);
}

bool PlumeCommandProcessor::SetupContext() {
  // Direct3D's doorbell wakes the worker through this (native_d3d.h).
  native_d3d::SetWake([this] { write_ptr_index_event_->Set(); });
  auto* renderer = PlumeRenderer::Get();
  if (!renderer) {
    REXLOG_ERROR("Renut: Plume command processor without a Plume renderer");
    return false;
  }
  device_ = renderer->device();
  queue_ = renderer->queue();
  miss_dump_directory_ = g_miss_dump_directory;
  store_ = std::make_unique<ShaderStore>();

  // Descriptor sets: the recompiled shaders' bindless heaps (register t0 in
  // spaces 0..2 and s0 in space 3 map to set N, binding 0).
  RenderDescriptorRange range_2d(RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures2D);
  RenderDescriptorRange range_3d(RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures3D);
  RenderDescriptorRange range_cube(RenderDescriptorRangeType::TEXTURE, 0, kMaxTexturesCube);
  RenderDescriptorRange range_samplers(RenderDescriptorRangeType::SAMPLER, 0, kMaxSamplers);
  RenderDescriptorSetDesc sets[] = {
      RenderDescriptorSetDesc(&range_2d, 1, true, kMaxTextures2D),
      RenderDescriptorSetDesc(&range_3d, 1, true, kMaxTextures3D),
      RenderDescriptorSetDesc(&range_cube, 1, true, kMaxTexturesCube),
      RenderDescriptorSetDesc(&range_samplers, 1, true, kMaxSamplers),
  };
  RenderPushConstantRange push(0, 0, 0, kPushBytes,
                               RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::GEOMETRY |
                                   RenderShaderStageFlag::PIXEL);
  layout_ = device_->createPipelineLayout(RenderPipelineLayoutDesc(&push, 1, sets, 4, false, true));
  set_2d_ = device_->createDescriptorSet(sets[0]);
  set_3d_ = device_->createDescriptorSet(sets[1]);
  set_cube_ = device_->createDescriptorSet(sets[2]);
  set_samplers_ = device_->createDescriptorSet(sets[3]);
  if (!layout_ || !set_2d_ || !set_3d_ || !set_cube_ || !set_samplers_) {
    REXLOG_ERROR("Renut: Plume pipeline layout unavailable");
    return false;
  }

  // Descriptor 0 is the fallback the rest of the heap falls back to; the
  // samplers a draw actually asks for are built on demand from its fetch
  // constants.
  samplers_[kSamplerDefault] = device_->createSampler(RenderSamplerDesc());
  if (!samplers_[kSamplerDefault]) {
    REXLOG_ERROR("Renut: Plume default sampler unavailable");
    return false;
  }
  set_samplers_->setSampler(kSamplerDefault, samplers_[kSamplerDefault].get());
  next_sampler_ = kSamplerDefault + 1;

  {
    const uint64_t total_ram = renutPhysicalMemoryBytes();
    const int32_t mode = REXCVAR_GET(renut_plume_low_memory);
    low_memory_ = mode < 0 ? total_ram < (6ull << 30) : mode > 0;
    if (low_memory_) {
      // Showdown Town has peaked at 126 MB of ring a frame on a big device; a
      // slice this size overflows there and costs a drain of the GPU each
      // time, which is the price of not holding 256 MB of RAM for it.
      upload_slice_size_ = 64ull << 20;
      kept_budget_ = 96ull << 20;
      max_texture_dim_ = 512;
      texture_budget_ = 256ull << 20;
    }
    if (const int32_t dim = REXCVAR_GET(renut_plume_texture_max_dim); dim > 0) max_texture_dim_ = uint32_t(dim);
    REXLOG_INFO("Renut: Plume memory profile {}: {} MB of RAM, ring {} MB a slice, kept streams {} MB, textures "
                "{} MB over {} px",
                low_memory_ ? "low" : "full", total_ram >> 20, upload_slice_size_ >> 20, kept_budget_ >> 20,
                texture_budget_ >> 20, max_texture_dim_);
  }
  {
    // Adreno has BC1-BC5 sampling, Mali does not: asked of the driver, since
    // Plume does not report it.
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(static_cast<VulkanDevice*>(device_)->physicalDevice, &features);
    bc_supported_ = features.textureCompressionBC == VK_TRUE;
    // ARM's vendor id.
    trail_device_default_ = static_cast<VulkanDevice*>(device_)->physicalDeviceProperties.vendorID == 0x13B5;
    trail_ = trail_device_default_;
  }
  bc_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_bc_textures), 0, 1));
  REXLOG_INFO("Renut: Plume BC textures: device {}, mode {}", bc_supported_ ? "samples BC1-BC5" : "has no BC",
              bc_mode_ ? "on" : "off");
  texture_quality_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_texture_quality), 0, 2));
  shader_cache_ = REXCVAR_GET(renut_plume_shader_cache);
  key_reuse_ = REXCVAR_GET(renut_plume_key_reuse);
  REXLOG_INFO("Renut: Plume texture quality: {}",
              texture_quality_ == 0 ? "full" : texture_quality_ == 1 ? "half" : "quarter");
  shadow_scale_ = 4 - uint32_t(std::clamp(REXCVAR_GET(renut_plume_shadow_quality), 0, 2));
  REXLOG_INFO("Renut: Plume shadow map resolution: {}/4 of the guest's", shadow_scale_);
  internal_scale_ = 4 - uint32_t(std::clamp(REXCVAR_GET(renut_plume_internal_resolution), 0, 2));
  REXLOG_INFO("Renut: Plume internal resolution: {}/4 of the guest's", internal_scale_);
  // One slice per frame in flight, after the permanent allocations.
  const uint64_t upload_bytes = upload_slice_size_ * kPlumeFramesInFlight + 4096;
  upload_ = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      upload_bytes, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT |
                        RenderBufferFlag::DEVICE_ADDRESSABLE | RenderBufferFlag::STORAGE));
  if (!upload_) {
    REXLOG_ERROR("Renut: Plume upload buffer unavailable");
    return false;
  }
  upload_data_ = static_cast<uint8_t*>(upload_->map());
  upload_address_ = upload_->getDeviceAddress();
  if (!CreateConstantBuffers()) {
    REXLOG_ERROR("Renut: Plume constant buffers unavailable");
    return false;
  }
  watch_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(WatchHitThunk, this);

  {
    auto* vulkan_device = static_cast<VulkanDevice*>(device_);
    timestamp_ns_ = vulkan_device->physicalDeviceProperties.limits.timestampPeriod;
    VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kMaxTimestamps * kPlumeFramesInFlight;
    if (vkCreateQueryPool(vulkan_device->vk, &info, nullptr, &timestamp_pool_) != VK_SUCCESS) {
      timestamp_pool_ = VK_NULL_HANDLE;
      REXLOG_WARN("Renut: Plume timestamp queries unavailable; no per-pass GPU time");
    }
    info.queryCount = kPairQueries * kPlumeFramesInFlight;
    if (timestamp_pool_ == VK_NULL_HANDLE ||
        vkCreateQueryPool(vulkan_device->vk, &info, nullptr, &pair_pool_) != VK_SUCCESS) {
      pair_pool_ = VK_NULL_HANDLE;
    }
  }
  for (auto& list : lists_) list = queue_->createCommandList();
  list_ = lists_[0].get();
  defer_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_defer_draws), 0, 2));
  hiz_cond_ = PlumeConditionalRenderingSupported();
  hiz_force_ = getenv("RENUT_HIZ_FORCE") != nullptr;
  hiz_verify_ = getenv("RENUT_HIZ_VERIFY") != nullptr;
  hiz_debug_ = getenv("RENUT_HIZ_DEBUG") != nullptr;
  if (hiz_verify_) {
    VkQueryPoolCreateInfo query_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_OCCLUSION;
    query_info.queryCount = kHizMaxQueries * kPlumeFramesInFlight;
    if (vkCreateQueryPool(static_cast<VulkanDevice*>(device_)->vk, &query_info, nullptr, &hiz_query_pool_) !=
        VK_SUCCESS) {
      hiz_query_pool_ = VK_NULL_HANDLE;
    }
  }
  hiz_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_hiz), 0, 4));
  fetch_spec_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_fetch_spec), 0, 1));
  depth_strip_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_depth_strip), 0, 1));
  rel_constants_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_rel_constants), 0, 2));
  no_shadows_ = REXCVAR_GET(renut_plume_no_shadows);
  no_bloom_ = REXCVAR_GET(renut_plume_no_bloom);
  adaptive_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_adaptive_patches), 0, 1));
  if (no_shadows_) shadow_min_indices_ = UINT32_MAX;
  fence_ = device_->createCommandFence();

  // Fallback textures: opaque white, for every slot without a texture yet.
  white_2d_ = device_->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  white_3d_ = device_->createTexture(RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  RenderTextureDesc cube = RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
                                                      RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE);
  white_cube_ = device_->createTexture(cube);
  RenderTextureViewDesc cube_view;
  cube_view.format = RenderFormat::R8G8B8A8_UNORM;
  cube_view.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
  white_cube_view_ = white_cube_->createTextureView(cube_view);

  rect_list_gs_ = device_->createShader(k_rect_list_geom, sizeof(k_rect_list_geom), "main",
                                        RenderShaderFormat::SPIRV);
  point_list_gs_ = device_->createShader(k_point_list_geom, sizeof(k_point_list_geom), "main",
                                         RenderShaderFormat::SPIRV);

  upload_used_ = 0;
  uint64_t white = Allocate(4 * 6, 256);
  std::memset(Pointer(white), 0xFF, 4 * 6);
  zero_offset_ = Allocate(64, 16);
  std::memset(Pointer(zero_offset_), 0, 64);
  // Everything after these belongs to a frame's slice.
  slice_origin_ = (upload_used_ + 255) & ~255ull;
  L()->begin();
  L()->barriers(RenderBarrierStage::COPY,
                  std::array{RenderTextureBarrier(white_2d_.get(), RenderTextureLayout::COPY_DEST),
                             RenderTextureBarrier(white_3d_.get(), RenderTextureLayout::COPY_DEST),
                             RenderTextureBarrier(white_cube_.get(), RenderTextureLayout::COPY_DEST)}
                      .data(),
                  3);
  auto footprint = RenderTextureCopyLocation::PlacedFootprint(upload_.get(), RenderFormat::R8G8B8A8_UNORM,
                                                              1, 1, 1, 64, white);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_2d_.get()), footprint);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_3d_.get()), footprint);
  for (uint32_t face = 0; face < 6; ++face) {
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_cube_.get(), 0, face), footprint);
  }
  L()->barriers(RenderBarrierStage::GRAPHICS,
                  std::array{RenderTextureBarrier(white_2d_.get(), RenderTextureLayout::SHADER_READ),
                             RenderTextureBarrier(white_3d_.get(), RenderTextureLayout::SHADER_READ),
                             RenderTextureBarrier(white_cube_.get(), RenderTextureLayout::SHADER_READ)}
                      .data(),
                  3);
  L()->end();
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  queue_->waitForCommandFence(fence_.get());
  for (uint32_t i = 0; i < kMaxTextures2D; ++i) {
    set_2d_->setTexture(i, white_2d_.get(), RenderTextureLayout::SHADER_READ);
  }
  for (uint32_t i = 0; i < kMaxTextures3D; ++i) {
    set_3d_->setTexture(i, white_3d_.get(), RenderTextureLayout::SHADER_READ);
  }
  for (uint32_t i = 0; i < kMaxTexturesCube; ++i) {
    set_cube_->setTexture(i, white_cube_.get(), RenderTextureLayout::SHADER_READ,
                          white_cube_view_.get());
  }
  lookup_budget_us_ = uint64_t(REXCVAR_GET(renut_plume_pipeline_lookup_ms)) * 1000;
  lookup_here_ = lookup_budget_us_ && PlumePipelineLookupSupported();
  REXLOG_INFO("Renut: Plume pipelines: {} compile, {}, boolean registers {}",
              REXCVAR_GET(renut_plume_async_pipelines) ? "async" : "sync",
              lookup_here_ ? fmt::format("cache lookups at the draw up to {} ms a frame", lookup_budget_us_ / 1000)
                           : std::string("no cache lookups at the draw"),
              REXCVAR_GET(renut_plume_bool_spec) ? "specialized" : "read from the shared block");
  validate_windows_ = g_validate_windows;
  dedupe_descriptors_ = g_dedupe_descriptors;
  if (dedupe_descriptors_) REXLOG_INFO("Renut: Plume 2D descriptors are written once per texture (plume-dedupe-desc)");
  writeback_ = REXCVAR_GET(renut_plume_memexport_readback);
  writeback_fast_ = REXCVAR_GET(renut_plume_memexport_fast);
  BeginFrame();
  REXLOG_INFO("Renut: Plume command processor ready ({} frames in flight, upload {} MB at {:X})",
              kPlumeFramesInFlight, upload_bytes >> 20, upload_address_);
  StartRecipes();
  device_watch_stop_ = false;
  device_watch_.thread = std::thread([this] {
    while (!device_watch_stop_.load()) {
      if (PlumeDeviceLost()) {
        ReportDeviceLost();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  });
  return true;
}

// The worker's loop. The lists the game's Direct3D published are run as they
// come (the doorbell wakes it); the primary ring is not read, only kept moving:
// a write pointer that advanced is taken as consumed and the read pointer is
// written back, which is all Direct3D looks at.
void PlumeCommandProcessor::RunWorkerLoop() {
  uint32_t idle = 0;
  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }
    bool worked = false;
    if (NativeWorkPending()) {
      RunNativeWork();
      worked = true;
    }
    const uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index != 0xBAADF00D && read_ptr_index_ != write_ptr_index) {
      read_ptr_index_ = write_ptr_index;
      if (read_ptr_writeback_ptr_) StoreBig(read_ptr_writeback_ptr_, read_ptr_index_);
      worked = true;
    }
    if (worked) {
      idle = 0;
      continue;
    }
    // Out of commands: spin for new ones, since waiting on the event costs more
    // than the wait is worth; it is waited on after a while to let the core rest.
    if (++idle > 500) {
      rex::thread::Wait(write_ptr_index_event_.get(), true, std::chrono::milliseconds(5));
    }
    rex::thread::MaybeYield();
  }
}

void PlumeCommandProcessor::ShutdownContext() {
  SyncRecorder();
  native_d3d::SetWake({});
  device_watch_stop_ = true;
  if (device_watch_.thread.joinable()) device_watch_.thread.join();
  StopPipelineWorker();
  StopRecipes();
  if (list_open_) {
    L()->end();
    list_open_ = false;
  }
  // Frames may still be running; nothing below may be freed under them.
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForAllSlots();
  pipelines_.clear();
  standins_.clear();
  pipeline_memo_.Clear();
  framebuffers_.clear();
  ++object_epoch_;
  targets_.clear();
  resolved_.clear();
  InvalidateResolvedLookups();
  ++export_epoch_;
  exports_.clear();
  patch_grids_.clear();
  if (watch_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(watch_handle_);
    watch_handle_ = nullptr;
  }
  ++stream_epoch_;
    kept_streams_.clear();
  kept_memo_.Clear();
  kept_bytes_ = 0;
  for (auto& retired : retired_buffers_) retired.clear();
  for (auto& buffer : writeback_buffers_) buffer.reset();
  for (auto& list : writebacks_) list.clear();
  shader_lookup_.clear();
  shader_memo_.Clear();
  shader_watch_memo_.Clear();
  shader_watch_.clear();
  shader_block_index_.clear();
  shaders_.clear();
  if (timestamp_pool_ != VK_NULL_HANDLE) {
    vkDestroyQueryPool(static_cast<VulkanDevice*>(device_)->vk, timestamp_pool_, nullptr);
    timestamp_pool_ = VK_NULL_HANDLE;
  }
  if (pair_pool_ != VK_NULL_HANDLE) {
    vkDestroyQueryPool(static_cast<VulkanDevice*>(device_)->vk, pair_pool_, nullptr);
    pair_pool_ = VK_NULL_HANDLE;
  }
  if (upload_) upload_->unmap();
  upload_.reset();
  {
    VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
    if (pack_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, pack_pipeline_, nullptr);
    if (pack_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, pack_layout_, nullptr);
    if (pack_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, pack_module_, nullptr);
    pack_pipeline_ = VK_NULL_HANDLE;
    pack_layout_ = VK_NULL_HANDLE;
    pack_module_ = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < 2; ++i) {
      if (patch_pipelines_[i] != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, patch_pipelines_[i], nullptr);
      if (patch_modules_[i] != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, patch_modules_[i], nullptr);
      patch_pipelines_[i] = VK_NULL_HANDLE;
      patch_modules_[i] = VK_NULL_HANDLE;
    }
    if (patch_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, patch_layout_, nullptr);
    HizDestroy();
    patch_layout_ = VK_NULL_HANDLE;
    for (auto& buffer : patch_out_) buffer.reset();
    stencil_staging_.reset();
    packed_staging_.reset();
    if (constants_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(vk_device, constants_pool_, nullptr);
    if (constants_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(vk_device, constants_set_layout_, nullptr);
    }
    constants_pool_ = VK_NULL_HANDLE;
    constants_set_layout_ = VK_NULL_HANDLE;
    constants_set_ = VK_NULL_HANDLE;
  }
}

// Set 4 of the pipeline layout: the stages' constant files and the shared
// block as dynamic uniform buffers over the upload ring. Plume has no dynamic
// descriptors, so the set is built in Vulkan and the layout Plume made is
// rebuilt with it appended - sets 0 to 3 and the push constants unchanged.
bool PlumeCommandProcessor::CreateConstantBuffers() {
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  auto* layout = static_cast<VulkanPipelineLayout*>(layout_.get());
  VkDescriptorSetLayoutBinding bindings[kConstantBindings] = {};
  for (uint32_t i = 0; i < kConstantBindings; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = kConstantBindings;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(vk_device, &set_info, nullptr, &constants_set_layout_) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkDescriptorSetLayout> set_layouts;
  for (VulkanDescriptorSetLayout* set_layout : layout->descriptorSetLayouts) set_layouts.push_back(set_layout->vk);
  if (set_layouts.size() != 4) return false;
  set_layouts.push_back(constants_set_layout_);
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = uint32_t(set_layouts.size());
  layout_info.pSetLayouts = set_layouts.data();
  layout_info.pushConstantRangeCount = uint32_t(layout->pushConstantRanges.size());
  layout_info.pPushConstantRanges = layout->pushConstantRanges.data();
  VkPipelineLayout extended = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &extended) != VK_SUCCESS) return false;
  vkDestroyPipelineLayout(vk_device, layout->vk, nullptr);
  layout->vk = extended;

  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kConstantBindings};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &constants_pool_) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = constants_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &constants_set_layout_;
  if (vkAllocateDescriptorSets(vk_device, &allocate, &constants_set_) != VK_SUCCESS) return false;
  const VkBuffer ring = static_cast<VulkanBuffer*>(upload_.get())->vk;
  VkDescriptorBufferInfo infos[kConstantBindings];
  static_assert(RENUT_SHARED_HOT_SIZE % 16 == 0 && RENUT_SHARED_SIZE % 16 == 0);
  for (uint32_t i = 0; i < kSharedHotBinding; ++i) infos[i] = {ring, 0, kConstantChunkSize};
  infos[kSharedHotBinding] = {ring, 0, RENUT_SHARED_HOT_SIZE};
  infos[kSharedColdBinding] = {ring, 0, RENUT_SHARED_SIZE - RENUT_SHARED_HOT_SIZE};
  // The whole vertex file, through both of its declarations.
  infos[kRelBinding] = {ring, 0, kStageConstantSize};
  infos[kRelBinding + 1] = {ring, 0, kStageConstantSize};
  VkWriteDescriptorSet writes[kConstantBindings] = {};
  for (uint32_t i = 0; i < kConstantBindings; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = constants_set_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(vk_device, kConstantBindings, writes, 0, nullptr);
  return true;
}

uint64_t PlumeCommandProcessor::Allocate(uint64_t size, uint64_t alignment) {
  const uint64_t limit = slice_base_ + upload_slice_size_;
  uint64_t offset = (upload_used_ + alignment - 1) & ~(alignment - 1);
  if (offset + size > limit) {
    ++stats_.overflow;
    Flush();
    offset = (upload_used_ + alignment - 1) & ~(alignment - 1);
    if (offset + size > limit) return UINT64_MAX;
  }
  upload_used_ = offset + size;
  return offset;
}

bool PlumeCommandProcessor::UploadGuest(uint32_t address, uint32_t size, xenos::Endian endian,
                                        uint64_t& offset) {
  if (!address || !size || size > (32u << 20)) return false;
  uint64_t key = (uint64_t(address) << 32) ^ (uint64_t(size) << 2) ^ uint64_t(endian);
  auto found = uploaded_streams_.find(key);
  if (found != uploaded_streams_.end() && !(skip_mask_ & 256)) {
    offset = found->second;
    return true;
  }
  offset = Allocate(size, 256);
  if (offset == UINT64_MAX) return false;
  CopySwapped(Pointer(offset), memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  stats_.stream_bytes += size;
  uploaded_streams_.emplace(key, offset);
  return true;
}

bool PlumeCommandProcessor::UploadRepacked(uint32_t address, uint32_t size, xenos::Endian endian,
                                           const std::pair<uint32_t, uint32_t>* elements,
                                           uint32_t element_count, uint64_t& offset) {
  if (!address || !size || size > (4u << 20)) return false;
  offset = Allocate(size, 256);
  if (offset == UINT64_MAX) return false;
  uint8_t* data = Pointer(offset);
  CopySwapped(data, memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t stride = elements[i].second;
    if (!stride) continue;
    for (uint64_t at = elements[i].first; at + 4 <= size; at += stride) {
      uint32_t word;
      std::memcpy(&word, data + at, 4);
      word = RepackFormat17(word);
      std::memcpy(data + at, &word, 4);
    }
  }
  stats_.stream_bytes += size;
  return true;
}

// Whether a memory export range touches the stream: what the GPU wrote there
// is not in guest memory.
bool PlumeCommandProcessor::ExportOverlaps(uint32_t address, uint32_t size) const {
  if (exports_.empty()) return false;
  auto first = exports_.upper_bound(address);
  if (first != exports_.begin()) --first;
  for (auto it = first; it != exports_.end() && it->first < uint64_t(address) + size; ++it) {
    const ExportRange& range = it->second;
    if (range.base < uint64_t(address) + size && address < uint64_t(range.base) + range.size) return true;
  }
  return false;
}

// The guest wrote into watched pages: which pages is all that is known, so
// every kept stream on them is dropped. Called with the guest's global lock
// held, from the thread that wrote, so it only records the range. Returning
// the range itself keeps rexglue from unwatching up to 4 MB around it, which
// would leave other kept streams there unwatched without a call.
std::pair<uint32_t, uint32_t> PlumeCommandProcessor::WatchHitThunk(void* context, uint32_t start,
                                                                   uint32_t length, bool) {
  auto* self = static_cast<PlumeCommandProcessor*>(context);
  {
    std::lock_guard<std::mutex> lock(self->watch_mutex_);
    self->watch_hits_.emplace_back(start, length);
  }
  self->watch_serial_.fetch_add(1, std::memory_order_release);
  return {start, length};
}

// A kept stream the guest wrote over: the buffer goes once the frames in flight
// are done with it, and it counts as dropped (see GuestStream).
void PlumeCommandProcessor::DropKept(KeptStream& kept) {
  ++stream_epoch_;
  // Earlier draws of this frame, and the frame before, may still read it.
  retired_buffers_[slot_].push_back(std::move(kept.buffer));
  kept_bytes_ -= kept.size;
  kept.valid = false;
  ++kept.drops;
  kept.dropped_frame = swaps_;
  ++stats_.kept_drops;
  ++kept_drop_events_;
}

void PlumeCommandProcessor::DrainWatchHits() {
  const uint32_t serial = watch_serial_.load(std::memory_order_acquire);
  if (serial == watch_seen_) return;
  watch_seen_ = serial;
  std::vector<std::pair<uint32_t, uint32_t>> hits;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    hits.swap(watch_hits_);
  }
  for (const auto& [start, length] : hits) {
    if (!length) continue;
    const uint32_t first = start >> 16, last = uint32_t((uint64_t(start) + length - 1) >> 16);
    auto touches = [&](uint32_t base, uint32_t end) { return base < start + uint64_t(length) && start < end; };
    auto drop_texture = [&](GuestTexture& texture) {
      if (texture.watched && (touches(texture.watch_begin[0], texture.watch_end[0]) ||
                              touches(texture.watch_begin[1], texture.watch_end[1]))) {
        texture.watched = false;
      }
    };
    auto drop_kept = [&](KeptStream& kept) {
      if (kept.valid && touches(kept.base, kept.base + kept.size)) DropKept(kept);
    };
    auto drop_shader = [&](ShaderWatch& entry) {
      if (entry.valid && touches(entry.base, entry.base + entry.bytes)) {
        entry.valid = false;
        ++entry.drops;
        entry.dropped_frame = swaps_;
        ++shader_cache_drops_;
      }
    };
    if (last - first > 1024) {
      // A huge range: every one is looked at.
      for (auto& [key, texture] : guest_textures_) drop_texture(texture);
      for (auto& [key, kept] : kept_streams_) drop_kept(kept);
      for (auto& [key, entry] : shader_watch_) drop_shader(entry);
      continue;
    }
    for (uint32_t block = first; block <= last; ++block) {
      auto textures = texture_block_index_.equal_range(block);
      for (auto it = textures.first; it != textures.second; ++it) {
        auto found = guest_textures_.find(it->second);
        if (found != guest_textures_.end()) drop_texture(found->second);
      }
      auto kepts = kept_block_index_.equal_range(block);
      for (auto it = kepts.first; it != kepts.second; ++it) {
        auto found = kept_streams_.find(it->second);
        if (found != kept_streams_.end()) drop_kept(found->second);
      }
      auto shaders = shader_block_index_.equal_range(block);
      for (auto it = shaders.first; it != shaders.second; ++it) {
        auto found = shader_watch_.find(it->second);
        if (found != shader_watch_.end()) drop_shader(found->second);
      }
    }
  }
  if (shader_block_index_.size() > 200000) {
    shader_block_index_.clear();
    for (auto& [key, entry] : shader_watch_) {
      if (entry.valid) IndexBlocks(shader_block_index_, entry.base, entry.bytes, key);
    }
  }
  // Entries of what is gone pile up: rebuilt from what is live.
  if (kept_block_index_.size() > 400000) {
    kept_block_index_.clear();
    for (auto& [key, kept] : kept_streams_) {
      if (kept.valid) IndexBlocks(kept_block_index_, kept.base, kept.size, key);
    }
  }
  if (texture_block_index_.size() > 400000) {
    texture_block_index_.clear();
    for (auto& [key, texture] : guest_textures_) {
      if (!texture.watched) continue;
      for (uint32_t which = 0; which < 2; ++which) {
        if (texture.watch_end[which] > texture.watch_begin[which]) {
          IndexBlocks(texture_block_index_, texture.watch_begin[which],
                      texture.watch_end[which] - texture.watch_begin[which], key);
        }
      }
    }
  }
  // Boxes the Hi-Z culling made from memory the guest has written.
  if (!hiz_boxes_.empty()) {
    for (const auto& [start, length] : hits) HizDropBoxes(start, length);
  }
}

bool PlumeCommandProcessor::RepackedStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                           const std::pair<uint32_t, uint32_t>* elements, uint32_t element_count,
                                           RenderBuffer*& buffer, uint64_t& offset) {
  buffer = upload_.get();
  if (!keep_streams_ || !address || !size || size > (4u << 20)) {
    return UploadRepacked(address, size, endian, elements, element_count, offset);
  }
  DrainWatchHits();
  uint64_t key = 0xC2B2AE3D27D4EB4Full ^ (uint64_t(address) << 20) ^ (uint64_t(size) << 3) ^ uint64_t(endian);
  for (uint32_t i = 0; i < element_count; ++i) {
    key = (key ^ ((uint64_t(elements[i].first) << 32) | elements[i].second)) * 0x100000001B3ull;
  }
  KeptStream& kept = kept_streams_[key];
  kept.last_used = swaps_;
  if (kept.valid && kept.base == address && kept.size == size) {
    buffer = kept.buffer.get();
    offset = 0;
    ++stats_.kept_hits;
    return true;
  }
  const bool dynamic = kept.drops >= 3 && swaps_ - kept.dropped_frame < 900;
  if (dynamic || kept_bytes_ + size > kept_budget_ ||
      memory_->GetPhysicalHeap()->QueryRangeAccess(address, address + size - 1) !=
          rex::memory::PageAccess::kReadWrite) {
    return UploadRepacked(address, size, endian, elements, element_count, offset);
  }
  auto made = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::DEVICE_ADDRESSABLE |
                RenderBufferFlag::STORAGE));
  if (!made) return UploadRepacked(address, size, endian, elements, element_count, offset);
  // Watch first, copy second: a write that lands during the copy still drops it.
  memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  auto* data = static_cast<uint8_t*>(made->map());
  CopySwapped(data, memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t stride = elements[i].second;
    if (!stride) continue;
    for (uint64_t at = elements[i].first; at + 4 <= size; at += stride) {
      uint32_t word;
      std::memcpy(&word, data + at, 4);
      word = RepackFormat17(word);
      std::memcpy(data + at, &word, 4);
    }
  }
  made->unmap();
  kept.buffer = std::move(made);
  kept.base = address;
  kept.size = size;
  kept.endian = uint32_t(endian);
  kept.made_frame = swaps_;
  kept.valid = true;
  IndexBlocks(kept_block_index_, address, size, key);
  kept_bytes_ += size;
  stats_.kept_bytes_made += size;
  stats_.stream_bytes += size;
  buffer = kept.buffer.get();
  offset = 0;
  return true;
}

bool PlumeCommandProcessor::GuestStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                        RenderBuffer*& buffer, uint64_t& offset) {
  // Streams are kept up to this much in all; past it they are copied as before.
  const uint64_t kKeptBudget = kept_budget_;
  buffer = upload_.get();
  if (!keep_streams_ || !address || !size || size > (32u << 20)) {
    return UploadGuest(address, size, endian, offset);
  }
  DrainWatchHits();
  // Kept by aligned window rather than by the exact stream: every draw of a
  // mesh names its own stretch of the same index buffer, and one entry per
  // stretch made tens of thousands of kept buffers, each looked up and each
  // checked against every write.
  // Native lists: a stream inside the vertex or index buffer it comes from is
  // kept as that whole buffer, which is exactly what the guest allocated -
  // no window running into pages beyond it.
  constexpr uint32_t kWindow = 16u << 10;
  uint32_t window_base = address & ~(kWindow - 1);
  uint64_t window_end =
      std::min<uint64_t>((uint64_t(address) + size + kWindow - 1) & ~uint64_t(kWindow - 1), 0x20000000ull);
  if (stream_object_.size && address >= stream_object_.base &&
      uint64_t(address) + size <= uint64_t(stream_object_.base) + stream_object_.size &&
      uint64_t(stream_object_.base) + stream_object_.size <= 0x20000000ull) {
    window_base = stream_object_.base;
    window_end = uint64_t(stream_object_.base) + stream_object_.size;
    ++object_streams_;
  } else {
    ++window_streams_;
  }
  if (window_end <= address + uint64_t(size) - 1 || window_end - window_base > (32u << 20)) {
    return UploadGuest(address, size, endian, offset);
  }
  const uint32_t window_size = uint32_t(window_end - window_base);
  const uint64_t key = (uint64_t(window_base) << 32) ^ (uint64_t(window_size) << 2) ^ uint64_t(endian);
  KeptStream* kept_node = kept_memo_.Find(key);
  if (!kept_node) {
    kept_node = &kept_streams_[key];
    kept_memo_.Put(key, kept_node);
  }
  KeptStream& kept = *kept_node;
  kept.last_used = swaps_;
  if (kept.valid) {
    buffer = kept.buffer.get();
    offset = address - window_base;
    ++stats_.kept_hits;
    return true;
  }
  // Written over three times: the guest rewrites it, and a watch would only
  // cost its thread a fault a frame. Tried again after a while.
  const bool dynamic = kept.drops >= 3 && swaps_ - kept.dropped_frame < 900;
  if (dynamic || kept_bytes_ + window_size > kKeptBudget) return UploadGuest(address, size, endian, offset);
  // The window runs past the stream to 16 KB boundaries, and a page there the
  // guest has not committed (or keeps read-only) gets no watch - rexglue only
  // watches writable pages, and committing one later tells nobody. A mesh the
  // game then streams into it was drawn from the stale copy: screen-sized
  // polygons with garbage textures, minutes into a level, until the window
  // happened to be dropped. Such a window goes through the ring, and is looked
  // at again once it would have stopped counting as dynamic.
  if (memory_->GetPhysicalHeap()->QueryRangeAccess(window_base, window_base + window_size - 1) !=
      rex::memory::PageAccess::kReadWrite) {
    kept.drops = std::max<uint32_t>(kept.drops, 3);
    kept.dropped_frame = swaps_;
    ++stats_.kept_unwatchable;
    return UploadGuest(address, size, endian, offset);
  }
  auto made = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      window_size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::DEVICE_ADDRESSABLE |
                RenderBufferFlag::STORAGE));
  if (!made) return UploadGuest(address, size, endian, offset);
  // Watch first, copy second: a write that lands during the copy still drops it.
  memory_->EnablePhysicalMemoryAccessCallbacks(window_base, window_size, true, false);
  auto* data = static_cast<uint8_t*>(made->map());
  CopySwapped(data, memory_->TranslatePhysical<const uint8_t*>(window_base), window_size, endian);
  made->unmap();
  kept.buffer = std::move(made);
  kept.base = window_base;
  kept.size = window_size;
  kept.endian = uint32_t(endian);
  kept.made_frame = swaps_;
  kept.valid = true;
  IndexBlocks(kept_block_index_, window_base, window_size, key);
  kept_bytes_ += window_size;
  stats_.kept_bytes_made += window_size;
  buffer = kept.buffer.get();
  offset = address - window_base;
  return true;
}

// Orders memory export against everything around it: shader stores and the
// copies into exported ranges before the vertex fetches, stores and copies
// that follow, and earlier fetches before later stores. It has to end the
// render pass; the pass reopens on the same framebuffer at the next draw.
void PlumeCommandProcessor::ExportBarrier() {
  auto* vulkan = static_cast<VulkanCommandList*>(L());
  vulkan->endActiveRenderPass();
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT;
  const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TRANSFER_BIT;
  vkCmdPipelineBarrier(vulkan->vk, stages, stages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  ++stats_.export_barriers;
  for (auto& [base, range] : exports_) range.written = range.read = false;
}

// The range a vertex shader exports into, holding [base, base + size). A new
// one starts as a copy of guest memory and takes over, with their contents,
// every range it overlaps.
PlumeCommandProcessor::ExportRange* PlumeCommandProcessor::GetExport(uint32_t base, uint32_t size,
                                                                     xenos::Endian endian) {
  uint32_t lo = base, hi = base + size;
  for (auto& [key, range] : exports_) {
    if (range.base <= lo && hi <= range.base + range.size) return &range;
  }
  std::vector<uint32_t> merged;
  for (bool grew = true; grew;) {
    grew = false;
    for (auto& [key, range] : exports_) {
      if (range.base < hi && lo < range.base + range.size &&
          std::find(merged.begin(), merged.end(), key) == merged.end()) {
        merged.push_back(key);
        lo = std::min(lo, range.base);
        hi = std::max(hi, range.base + range.size);
        grew = true;
      }
    }
  }
  ExportRange fresh;
  fresh.base = lo;
  fresh.size = hi - lo;
  // Held the way every vertex stream is, k8in32, whatever the endianness of
  // the exports into it: renutMemExport converts each store to that form.
  fresh.endian = xenos::Endian::k8in32;
  fresh.buffer = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
      fresh.size, RenderBufferFlag::VERTEX | RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT |
                      RenderBufferFlag::DEVICE_ADDRESSABLE));
  if (!fresh.buffer) return nullptr;
  fresh.address = fresh.buffer->getDeviceAddress();
  const uint64_t offset = Allocate(fresh.size, 256);
  if (offset == UINT64_MAX) return nullptr;
  const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(lo);
  fresh.snapshot.assign(memory, memory + fresh.size);
  CopySwapped(Pointer(offset), memory, fresh.size, fresh.endian);
  bool pending = false;
  for (uint32_t key : merged) pending |= exports_[key].written;
  if (pending) ExportBarrier();
  L()->copyBufferRegion(RenderBufferReference(fresh.buffer.get(), 0),
                          RenderBufferReference(upload_.get(), offset), fresh.size);
  if (!merged.empty()) {
    // The guest's bytes first, then what the GPU wrote over them.
    ExportBarrier();
    for (uint32_t key : merged) {
      ExportRange& old = exports_[key];
      L()->copyBufferRegion(RenderBufferReference(fresh.buffer.get(), old.base - lo),
                              RenderBufferReference(old.buffer.get(), 0), old.size);
      retired_buffers_[slot_].push_back(std::move(old.buffer));
      ++export_epoch_;
      exports_.erase(key);
    }
  }
  fresh.written = true;
  fresh.checked_frame = swaps_;
  fresh.exported_frame = swaps_;
  ++stats_.export_ranges;
  static uint32_t logged = 0;
  if (logged < 64) {
    ++logged;
    REXLOG_INFO("Renut: Plume export range {:08X}+{:X} endian {} (asked {:08X}+{:X}, merged {})", lo,
                fresh.size, uint32_t(endian), base, size, merged.size());
  }
  ++export_epoch_;
  return &exports_.emplace(lo, std::move(fresh)).first->second;
}

void PlumeCommandProcessor::RecordWritebacks() {
  auto& list = writebacks_[slot_];
  list.clear();
  if (!writeback_) return;
  uint64_t total = 0;
  for (const auto& [key, range] : exports_) {
    if (range.exported_frame == swaps_) total += range.size;
  }
  if (!total) return;
  if (total > writeback_capacity_[slot_]) {
    // The slot's last frame has finished (BeginFrame waited for it), so the
    // old buffer is free to go.
    const uint64_t capacity = std::max<uint64_t>(total + total / 2, 1u << 20);
    writeback_buffers_[slot_] = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(capacity));
    writeback_capacity_[slot_] = writeback_buffers_[slot_] ? capacity : 0;
    if (!writeback_buffers_[slot_]) return;
  }
  ExportBarrier();
  uint64_t at = 0;
  for (const auto& [key, range] : exports_) {
    if (range.exported_frame != swaps_) continue;
    L()->copyBufferRegion(RenderBufferReference(writeback_buffers_[slot_].get(), at),
                            RenderBufferReference(range.buffer.get(), 0), range.size);
    list.push_back({range.base, range.size, at, range.buffer.get()});
    at += range.size;
  }
}

void PlumeCommandProcessor::ApplyWritebacks(uint32_t slot) {
  auto& list = writebacks_[slot];
  if (list.empty()) return;
  if (!writeback_) {
    list.clear();
    return;
  }
  const auto* data = static_cast<const uint8_t*>(writeback_buffers_[slot]->map());
  constexpr uint32_t kBlock = 256;
  alignas(16) uint8_t block[kBlock];
  for (const Writeback& w : list) {
    auto found = exports_.find(w.base);
    if (found == exports_.end() || found->second.buffer.get() != w.buffer ||
        found->second.size != w.size) {
      continue;
    }
    ExportRange& range = found->second;
    uint8_t* guest = memory_->TranslatePhysical<uint8_t*>(range.base);
    for (uint32_t at = 0; at < range.size; at += kBlock) {
      const uint32_t length = std::min(kBlock, range.size - at);
      // A block the CPU wrote since is left alone: its bytes win, and the
      // next RefreshExport takes them in.
      if (std::memcmp(guest + at, range.snapshot.data() + at, length) != 0) continue;
      CopySwapped(block, data + w.offset + at, length, range.endian);
      if (std::memcmp(block, range.snapshot.data() + at, length) == 0) continue;
      std::memcpy(guest + at, block, length);
      std::memcpy(range.snapshot.data() + at, block, length);
    }
  }
  writeback_buffers_[slot]->unmap();
  list.clear();
}

// Brings in what the CPU wrote to an exported range since it was last looked
// at, once a frame.
void PlumeCommandProcessor::RefreshExport(ExportRange& range) {
  if (range.checked_frame == swaps_) return;
  range.checked_frame = swaps_;
  const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(range.base);
  if (std::memcmp(memory, range.snapshot.data(), range.size) == 0) return;
  if (range.written) ExportBarrier();
  constexpr uint32_t kBlock = 256;
  auto differs = [&](uint32_t at) {
    return std::memcmp(memory + at, range.snapshot.data() + at, std::min(kBlock, range.size - at)) != 0;
  };
  for (uint32_t at = 0; at < range.size;) {
    if (!differs(at)) {
      at += kBlock;
      continue;
    }
    uint32_t end = at;
    while (end < range.size && differs(end)) end += kBlock;
    end = std::min(end, range.size);
    const uint64_t offset = Allocate(end - at, 256);
    if (offset == UINT64_MAX) break;
    CopySwapped(Pointer(offset), memory + at, end - at, range.endian);
    L()->copyBufferRegion(RenderBufferReference(range.buffer.get(), at),
                            RenderBufferReference(upload_.get(), offset), end - at);
    std::memcpy(range.snapshot.data() + at, memory + at, end - at);
    at = end;
  }
  range.written = true;
  ++stats_.export_refreshes;
}

// Where a draw reads a vertex stream: the exported range that holds it whole,
// or the upload ring - with the exported stretches of a stream that is only
// partly exported copied over the guest's bytes.
bool PlumeCommandProcessor::BindStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                       RenderBuffer*& buffer, uint64_t& offset) {
  buffer = upload_.get();
  if (exports_.empty()) return GuestStream(address, size, endian, buffer, offset);
  std::vector<ExportRange*> overlapping;
  // The ranges are disjoint (GetExport merges whatever a new one overlaps) and
  // keyed by their base, so only the one starting at or before the stream and
  // those starting inside it can touch it. Walking the whole map for every
  // stream of every draw was ~4% of this thread.
  auto first = exports_.upper_bound(address);
  if (first != exports_.begin()) --first;
  for (auto it = first; it != exports_.end() && it->first < uint64_t(address) + size; ++it) {
    ExportRange& range = it->second;
    if (address >= range.base && uint64_t(address) + size <= uint64_t(range.base) + range.size) {
      RefreshExport(range);
      if (range.written) ExportBarrier();
      range.read = true;
      buffer = range.buffer.get();
      offset = address - range.base;
      ++stats_.export_reads;
      Trace("  stream {:08X}+{:X} from export {:08X}+{:X}", address, size, range.base, range.size);
      return true;
    }
    if (range.base < address + size && address < range.base + range.size) overlapping.push_back(&range);
  }
  if (overlapping.empty()) return GuestStream(address, size, endian, buffer, offset);
  // A stream that is mostly exported is typically the pool the particle
  // emitters export into piece by piece, drawn whole. Growing a range over it
  // lets every later draw read it straight away instead of piecing it
  // together again, two barriers each time. One that is mostly guest data is
  // pieced together instead: a range only catches up with the CPU's writes
  // once a frame, and the rest of such a buffer may be written between draws.
  uint32_t exported = 0;
  for (ExportRange* range : overlapping) {
    exported += std::min(address + size, range->base + range->size) - std::max(address, range->base);
  }
  if (size <= (4u << 20) && exported * 2 >= size) {
    if (ExportRange* range = GetExport(address, size, endian)) {
      RefreshExport(*range);
      ExportBarrier();
      range->read = true;
      buffer = range->buffer.get();
      offset = address - range->base;
      ++stats_.export_reads;
      return true;
    }
  }
  if (!address || !size || size > (32u << 20)) return false;
  offset = Allocate(size, 256);
  if (offset == UINT64_MAX) return false;
  CopySwapped(Pointer(offset), memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  bool pending = false;
  for (ExportRange* range : overlapping) {
    RefreshExport(*range);
    pending |= range->written;
  }
  if (pending) ExportBarrier();
  for (ExportRange* range : overlapping) {
    const uint32_t lo = std::max(address, range->base);
    const uint32_t hi = std::min(address + size, range->base + range->size);
    L()->copyBufferRegion(RenderBufferReference(upload_.get(), offset + (lo - address)),
                            RenderBufferReference(range->buffer.get(), lo - range->base), hi - lo);
  }
  ExportBarrier();
  for (ExportRange* range : overlapping) range->read = true;
  ++stats_.export_merges;
  return true;
}

// Puts `source` in the upload ring unless an earlier draw already left the
// same bytes there - see the shadow buffers.
bool PlumeCommandProcessor::CommitConstants(uint8_t* shadow, const void* source, size_t size,
                                            uint64_t& offset, bool* written) {
  // Nothing written into the block since it was last placed: the same bytes,
  // without comparing them.
  if (offset != UINT64_MAX && written && !*written && !(skip_mask_ & 4194304)) {
    ++stats_.constants_reused;
    return true;
  }
  if (written) *written = false;
  if (offset != UINT64_MAX && !(skip_mask_ & 4194304) && std::memcmp(shadow, source, size) == 0) {
    ++stats_.constants_reused;
    return true;
  }
  // Allocate can flush the ring, which drops every offset including this one.
  const uint64_t at = Allocate(size, 256);
  if (at == UINT64_MAX) return false;
  std::memcpy(shadow, source, size);
  std::memcpy(Pointer(at), source, size);
  stats_.constant_bytes += size;
  offset = at;
  return true;
}

// A triangle patch's grid has level + 1 - row points in row `row`, weighted by
// their distance along each edge; a quad patch's is (level + 1)^2 points. The
// triangles are the ones, in the order, the unindexed expansion drew.
const PlumeCommandProcessor::PatchGrid* PlumeCommandProcessor::GetPatchGrid(bool quad, uint32_t level,
                                                                            uint32_t patches) {
  PatchGrid& grid = patch_grids_[level | (quad ? 0x100u : 0u)];
  if (grid.patches >= patches) return &grid;
  const uint32_t capacity = std::max(patches, grid.patches * 2);
  const uint32_t per_patch = quad ? (level + 1) * (level + 1) : (level + 1) * (level + 2) / 2;
  const uint32_t indices_per_patch = level * level * (quad ? 6 : 3);
  auto points = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      uint64_t(capacity) * per_patch * 4 * sizeof(float), RenderBufferFlag::VERTEX));
  auto indices = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      uint64_t(capacity) * indices_per_patch * sizeof(uint32_t), RenderBufferFlag::INDEX));
  if (!points || !indices) return grid.patches ? &grid : nullptr;
  auto* point = static_cast<float*>(points->map());
  auto* index = static_cast<uint32_t*>(indices->map());
  const float step = 1.0f / float(level);
  // The first point of each row of a triangle patch.
  auto row_start = [&](uint32_t row) { return row * (level + 1) - row * (row - 1) / 2; };
  for (uint32_t patch = 0; patch < capacity; ++patch) {
    const uint32_t first = patch * per_patch;
    if (quad) {
      // A patch-indexed quad domain shader takes the patch index in r0.x and
      // the domain location in r0.yz, and r1.x is zero; the recompiled shader
      // reads r0 as iDomainPatch.zyx and r1.x as its w.
      for (uint32_t v = 0; v <= level; ++v) {
        for (uint32_t u = 0; u <= level; ++u) {
          *point++ = float(v) * step;
          *point++ = float(u) * step;
          *point++ = float(patch);
          *point++ = 0.0f;
        }
      }
      auto at = [&](uint32_t u, uint32_t v) { return first + v * (level + 1) + u; };
      for (uint32_t v = 0; v < level; ++v) {
        for (uint32_t u = 0; u < level; ++u) {
          const uint32_t quad_indices[6] = {at(u, v),     at(u + 1, v),     at(u + 1, v + 1),
                                            at(u, v),     at(u + 1, v + 1), at(u, v + 1)};
          std::memcpy(index, quad_indices, sizeof(quad_indices));
          index += 6;
        }
      }
    } else {
      for (uint32_t row = 0; row <= level; ++row) {
        for (uint32_t col = 0; col + row <= level; ++col) {
          *point++ = float(col) * step;
          *point++ = float(level - row - col) * step;
          *point++ = float(row) * step;
          *point++ = float(patch);
        }
      }
      auto at = [&](uint32_t row, uint32_t col) { return first + row_start(row) + col; };
      for (uint32_t row = 0; row < level; ++row) {
        for (uint32_t col = 0; col + row < level; ++col) {
          *index++ = at(row, col);
          *index++ = at(row, col + 1);
          *index++ = at(row + 1, col);
          if (col + row + 1 < level) {
            *index++ = at(row, col + 1);
            *index++ = at(row + 1, col + 1);
            *index++ = at(row + 1, col);
          }
        }
      }
    }
  }
  points->unmap();
  indices->unmap();
  // Draws recorded earlier this frame may still read the old grid.
  if (grid.points) retired_buffers_[slot_].push_back(std::move(grid.points));
  if (grid.indices) retired_buffers_[slot_].push_back(std::move(grid.indices));
  grid.points = std::move(points);
  grid.indices = std::move(indices);
  grid.patches = capacity;
  grid.points_per_patch = per_patch;
  grid.indices_per_patch = indices_per_patch;
  REXLOG_INFO("Renut: Plume patch grid {} level {} for {} patches ({} points, {} indices each)",
              quad ? "quad" : "triangle", level, capacity, per_patch, indices_per_patch);
  return &grid;
}

void PlumeCommandProcessor::WriteTimestamp() {
  if (!OnRecorder()) SyncRecorder();
  // plume-skip bit 67108864 turns the per-pass timing off. It writes ~138
  // timestamps a frame, mostly at render pass boundaries, and a timestamp there
  // can make a tiler flush - so its own cost has to be measurable.
  if (timestamp_pool_ == VK_NULL_HANDLE || (skip_mask_ & 67108864) ||
      timestamps_used_ >= kMaxTimestamps) {
    return;
  }
  vkCmdWriteTimestamp(static_cast<VulkanCommandList*>(L())->vk,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool_,
                      slot_ * kMaxTimestamps + timestamps_used_);
  ++timestamps_used_;
}

// Folds the timings a finished frame left behind into the running totals. The
// interval between two marks is the pass that ran between them.
void PlumeCommandProcessor::ReadTimestamps(uint32_t slot) {
  const uint32_t count = timestamp_count_[slot];
  timestamp_count_[slot] = 0;
  if (timestamp_pool_ == VK_NULL_HANDLE || count < 2) return;
  std::array<uint64_t, kMaxTimestamps> ticks{};
  if (vkGetQueryPoolResults(static_cast<VulkanDevice*>(device_)->vk, timestamp_pool_,
                            slot * kMaxTimestamps, count, count * sizeof(uint64_t), ticks.data(),
                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  const auto& marks = pass_marks_[slot];
  for (uint32_t i = 0; i + 1 < count; ++i) {
    if (ticks[i + 1] <= ticks[i]) continue;
    const uint64_t ns = uint64_t(double(ticks[i + 1] - ticks[i]) * timestamp_ns_);
    gpu_ns_ += ns;
    // Mark i opened the pass that the interval measures; the first interval is
    // whatever ran before any framebuffer was bound.
    uint64_t key = 0;
    uint32_t draws = 0;
    if (i < marks.size()) {
      const PassMark& m = marks[i];
      key = (uint64_t(m.width) << 32) | (uint64_t(m.height) << 8) | (uint64_t(m.colors) << 1) |
            uint64_t(m.depth);
      key |= uint64_t(m.resolve) << 62;
      draws = m.draws;
    }
    PassTotal& total = pass_totals_[key];
    total.ns += ns;
    total.passes += 1;
    total.draws += draws;
  }
}

// A profiled draw's entry and its first timestamp. False when the frame has no
// room for another pair, in which case the draw simply goes unmeasured.
bool PlumeCommandProcessor::PairBegin(const DrawPacket& pk) {
  if (pair_used_ + 2 > kPairQueries) return false;
  PairMark mark;
  mark.vs = pk.vs_hash;
  mark.ps = pk.ps_hash;
  mark.count = pk.count;
  mark.colors = uint8_t(pk.pass_colors);
  mark.depth = pk.pass_depth ? 1 : 0;
  mark.kind = uint8_t(pk.kind);
  mark.switched = bound_pipeline_ != pk.pipeline ? 1 : 0;
  pair_marks_[slot_].push_back(mark);
  PairStamp();
  return true;
}

void PlumeCommandProcessor::PairStamp() {
  vkCmdWriteTimestamp(static_cast<VulkanCommandList*>(list_)->vk,
                      pairprof_mode_ == 2 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      pair_pool_, slot_ * kPairQueries + pair_used_);
  ++pair_used_;
}

// Folds a captured frame into the per-pair sums, and writes the report once the
// last frame of the capture has been read.
void PlumeCommandProcessor::ReadPairTimestamps(uint32_t slot) {
  if (!pair_pending_[slot]) return;
  pair_pending_[slot] = false;
  const uint32_t count = pair_count_[slot];
  pair_count_[slot] = 0;
  const auto& marks = pair_marks_[slot];
  if (pair_pool_ != VK_NULL_HANDLE && count >= 2 && marks.size() * 2 == count) {
    std::vector<uint64_t> ticks(count);
    if (vkGetQueryPoolResults(static_cast<VulkanDevice*>(device_)->vk, pair_pool_, slot * kPairQueries, count,
                              count * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      auto ns = [&](uint64_t from, uint64_t to) {
        return to > from ? uint64_t(double(to - from) * timestamp_ns_) : uint64_t(0);
      };
      for (size_t i = 0; i < marks.size(); ++i) {
        const PairMark& m = marks[i];
        const uint64_t span = ns(ticks[2 * i], ticks[2 * i + 1]);
        PairTotal& total = pair_totals_[{m.vs, m.ps}];
        total.ns += span;
        ++total.draws;
        total.vertices += m.count;
        total.switches += m.switched;
        total.colors = m.colors;
        total.depth = m.depth;
        pair_span_ns_ += span;
        if (i + 1 < marks.size()) pair_gap_ns_ += ns(ticks[2 * i + 1], ticks[2 * i + 2]);
      }
      pair_frame_ns_ += ns(ticks[0], ticks[count - 1]);
      ++pair_frames_;
    }
  }
  if (pairprof_frames_left_ == 0 && std::none_of(pair_pending_.begin(), pair_pending_.end(), [](bool p) { return p; })) {
    WritePairReport();
  }
}

void PlumeCommandProcessor::WritePairReport() {
  const uint32_t frames = pair_frames_;
  const uint32_t mode = pairprof_mode_;
  const uint64_t span_ns = pair_span_ns_, gap_ns = pair_gap_ns_, frame_ns = pair_frame_ns_;
  std::map<std::pair<uint64_t, uint64_t>, PairTotal> totals;
  totals.swap(pair_totals_);
  pair_frames_ = 0;
  pair_span_ns_ = pair_gap_ns_ = pair_frame_ns_ = 0;
  if (!frames || miss_dump_directory_.empty()) return;
  const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));

  struct Row {
    uint64_t vs, ps;
    PairTotal total;
  };
  std::vector<Row> rows;
  PairTotal depth_only, colour;
  for (const auto& [key, total] : totals) {
    rows.push_back({key.first, key.second, total});
    PairTotal& into = total.colors ? colour : depth_only;
    into.ns += total.ns;
    into.draws += total.draws;
    into.switches += total.switches;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.total.ns > b.total.ns; });
  const double per = 1.0 / double(frames);
  const double all = span_ns ? double(span_ns) : 1.0;
  auto us = [&](uint64_t ns) { return double(ns) / 1000.0 * per; };
  auto pct = [&](uint64_t ns) { return 100.0 * double(ns) / all; };

  std::string out;
  out += fmt::format("# plume-pairprof mode {} ({}), {} frames, {} pairs, timestamp period {:.2f} ns\n", mode,
                     mode == 2 ? "bottom of pipe: each draw alone" : "top of pipe: command processor clock",
                     frames, rows.size(), timestamp_ns_);
  uint64_t draws = depth_only.draws + colour.draws;
  out += fmt::format("# per frame: {:.0f} draws, spans {:.0f} us, between draws {:.0f} us, first stamp to last {:.0f} us\n",
                     double(draws) * per, us(span_ns), us(gap_ns), us(frame_ns));
  out += fmt::format("# depth-only: {:.0f} draws {:.0f} us ({:.1f}%, {:.0f}% switch pipeline) | colour: {:.0f} draws {:.0f} us ({:.1f}%, {:.0f}% switch)\n",
                     double(depth_only.draws) * per, us(depth_only.ns), pct(depth_only.ns),
                     depth_only.draws ? 100.0 * double(depth_only.switches) / double(depth_only.draws) : 0.0,
                     double(colour.draws) * per, us(colour.ns), pct(colour.ns),
                     colour.draws ? 100.0 * double(colour.switches) / double(colour.draws) : 0.0);
  out += "# rank    pct   us/frame  draws/frame  avg_us  avg_verts  switch%  c d  vs               ps\n";
  const size_t shown = std::min<size_t>(rows.size(), 150);
  for (size_t i = 0; i < shown; ++i) {
    const Row& r = rows[i];
    const PairTotal& t = r.total;
    out += fmt::format("{:5d} {:6.2f} {:10.1f} {:11.2f} {:7.2f} {:10.0f} {:7.0f}  {} {}  {:016X} {:016X}\n", i + 1,
                       pct(t.ns), us(t.ns), double(t.draws) * per, double(t.ns) / 1000.0 / double(t.draws),
                       double(t.vertices) / double(t.draws), 100.0 * double(t.switches) / double(t.draws),
                       t.colors, t.depth, r.vs, r.ps);
  }
  for (int side = 0; side < 2; ++side) {
    std::map<uint64_t, PairTotal> by;
    for (const Row& r : rows) {
      PairTotal& into = by[side == 0 ? r.vs : r.ps];
      into.ns += r.total.ns;
      into.draws += r.total.draws;
      into.vertices += r.total.vertices;
      into.switches += r.total.switches;
    }
    std::vector<std::pair<uint64_t, PairTotal>> ranked(by.begin(), by.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    out += fmt::format("# by {} shader ({} of them)\n", side == 0 ? "vertex" : "pixel", ranked.size());
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 100); ++i) {
      const PairTotal& t = ranked[i].second;
      out += fmt::format("{:5d} {:6.2f} {:10.1f} {:11.2f} {:7.2f}  {:016X}\n", i + 1, pct(t.ns), us(t.ns),
                         double(t.draws) * per, double(t.ns) / 1000.0 / double(t.draws), ranked[i].first);
    }
  }
  if (FILE* file = std::fopen((root + "/plume-pairprof.txt").c_str(), "w")) {
    std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
  }
  REXLOG_INFO("Renut: Plume pair profile written: {} frames, {} pairs, {:.0f} draws a frame", frames, rows.size(),
              double(draws) * per);
}

// Moves to the next frame in flight and opens its command list. Waiting here,
// rather than after the submission that ended the last frame, is what lets the
// GPU run a frame while this one is recorded.
// Nothing in these caches was ever freed. Every trip between levels added the
// new level's textures on top of the old ones until the 4096 2D descriptors ran
// out (about four trips): from then on every new texture sampled the white one
// in slot 0 and the picture collapsed to white. The 16 cube descriptors ran
// out long before that. Whatever has not been sampled for a while goes; it is
// decoded again if it comes back, and a resolve is only dropped when it has
// been neither written nor read.
void PlumeCommandProcessor::EvictIdleTextures() {
  const uint32_t free_2d = kMaxTextures2D - next_descriptor_ + uint32_t(free_descriptors_.size());
  const uint32_t free_3d = kMaxTextures3D - next_3d_descriptor_ + uint32_t(free_3d_descriptors_.size());
  const uint32_t free_cube = kMaxTexturesCube - next_cube_descriptor_ + uint32_t(free_cube_descriptors_.size());
  // About a minute at the game's frame rate, a few seconds when a set runs low.
  auto idle_for = [&](bool short_of) {
    return short_of ? uint64_t(60) : (low_memory_ ? uint64_t(300) : uint64_t(900));
  };
  auto idle = [&](uint64_t frame, uint64_t limit) { return frame == ~0ull || swaps_ - frame >= limit; };
  uint32_t evicted = 0;

  const uint64_t limit_2d = idle_for(free_2d < 512);
  for (auto it = guest_textures_.begin(); it != guest_textures_.end();) {
    if (!idle(it->second.checked_frame, limit_2d)) { ++it; continue; }
    RetireGuestTexture(it->second);
    it = guest_textures_.erase(it);
    ++evicted;
  }
  // A budget on top of the idle rule, for a host that cannot wait for a texture
  // to have gone unused: the oldest go first, never one drawn in the last few
  // frames (the frames in flight may still read it).
  if (texture_budget_) {
    uint64_t bytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> by_age;  // frame last checked, key
    for (const auto& [key, t] : guest_textures_) {
      if (!t.texture) continue;
      bytes += HostTextureBytes(t);
      by_age.emplace_back(t.checked_frame == ~0ull ? 0 : t.checked_frame, key);
    }
    if (bytes > texture_budget_) {
      std::sort(by_age.begin(), by_age.end());
      for (const auto& [frame, key] : by_age) {
        if (bytes <= texture_budget_ || swaps_ - frame < 3) break;
        auto it = guest_textures_.find(key);
        if (it == guest_textures_.end()) continue;
        bytes -= HostTextureBytes(it->second);
        RetireGuestTexture(it->second);
        guest_textures_.erase(it);
        ++evicted;
      }
    }
  }
  // Once, not per erased entry: the memo is 64 KB.
  texture_memo_.Clear();
  ++object_epoch_;
  const uint64_t limit_3d = idle_for(free_3d < 16);
  for (auto* map : {&guest_stacks_, &guest_volumes_}) {
    for (auto it = map->begin(); it != map->end();) {
      if (!idle(it->second.checked_frame, limit_3d)) { ++it; continue; }
      if (it->second.texture) retired_slots_[slot_].push_back(std::move(it->second.texture));
      if (it->second.descriptor) retired_3d_descriptor_slots_[slot_].push_back(it->second.descriptor);
      it = map->erase(it);
      ++evicted;
    }
  }
  const uint64_t limit_cube = idle_for(free_cube < 4);
  for (auto it = guest_cubes_.begin(); it != guest_cubes_.end();) {
    if (!idle(it->second.checked_frame, limit_cube)) { ++it; continue; }
    if (it->second.view) retired_views_[slot_].push_back(std::move(it->second.view));
    if (it->second.texture) retired_slots_[slot_].push_back(std::move(it->second.texture));
    if (it->second.descriptor) retired_cube_descriptor_slots_[slot_].push_back(it->second.descriptor);
    it = guest_cubes_.erase(it);
    ++evicted;
  }
  const uint64_t limit_resolved = free_2d < 512 ? 120 : 1800;
  for (auto it = resolved_.begin(); it != resolved_.end();) {
    Resolved& r = it->second;
    if (!idle(r.written_frame, limit_resolved) || !idle(r.checked_frame, limit_resolved)) { ++it; continue; }
    RetireSwizzled(r);
    if (r.alias) RetireFramebuffers();
    RetireTexture(r.texture, r.descriptor);
    RetireTexture(r.fitted, r.fitted_descriptor);
    it = resolved_.erase(it);
    InvalidateResolvedLookups();
    ++evicted;
  }
  bool kept_erased = false;
  for (auto it = kept_streams_.begin(); it != kept_streams_.end();) {
    if (swaps_ - it->second.last_used < 600) { ++it; continue; }
    if (it->second.buffer) {
      retired_buffers_[slot_].push_back(std::move(it->second.buffer));
      kept_bytes_ -= it->second.size;
    }
    it = (++stream_epoch_, kept_streams_.erase(it));
    kept_erased = true;
  }
  if (kept_erased) kept_memo_.Clear();
  if (evicted) {
    REXLOG_INFO("Renut: Plume evicted {} idle textures (free descriptors 2D {} 3D {} cube {})", evicted,
                free_2d, free_3d, free_cube);
  }
}

void PlumeCommandProcessor::BeginFrame() {
  SyncRecorder();
  slot_ = (slot_ + 1) % kPlumeFramesInFlight;
  // The slot's command list, ring slice and retired resources are all still in
  // use until the frame that last held it has finished.
  const auto wait_start = std::chrono::steady_clock::now();
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForSlot(slot_);
  stats_.slot_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - wait_start)
                                      .count());
  ReadTimestamps(slot_);
  ReadPairTimestamps(slot_);
  HizReadResults(slot_);
  hiz_tests_frame_ = hiz_tests_remembered_ = 0;
  hiz_learn_left_ = 3;
  ApplyWritebacks(slot_);
  list_ = lists_[slot_].get();
  slice_base_ = slice_origin_ + uint64_t(slot_) * upload_slice_size_;
  for (uint32_t index : retired_descriptor_slots_[slot_]) {
    set_2d_->setTexture(index, white_2d_.get(), RenderTextureLayout::SHADER_READ);
    written_2d_[index] = {white_2d_.get(), nullptr};
    free_descriptors_.push_back(index);
  }
  retired_descriptor_slots_[slot_].clear();
  for (uint32_t index : retired_3d_descriptor_slots_[slot_]) {
    set_3d_->setTexture(index, white_3d_.get(), RenderTextureLayout::SHADER_READ);
    free_3d_descriptors_.push_back(index);
  }
  retired_3d_descriptor_slots_[slot_].clear();
  for (uint32_t index : retired_cube_descriptor_slots_[slot_]) {
    set_cube_->setTexture(index, white_cube_.get(), RenderTextureLayout::SHADER_READ, white_cube_view_.get());
    free_cube_descriptors_.push_back(index);
  }
  retired_cube_descriptor_slots_[slot_].clear();
  if (retire_delay_) {
    // Debug: hold what the frames in flight no longer need a while longer.
    if (retire_mask_ & 1) for (auto& object : retired_framebuffer_slots_[slot_]) delayed_buffers_.emplace_back(swaps_, std::shared_ptr<void>(std::move(object)));
    if (retire_mask_ & 2) for (auto& object : retired_slots_[slot_]) delayed_buffers_.emplace_back(swaps_, std::shared_ptr<void>(std::move(object)));
    if (retire_mask_ & 4) for (auto& object : retired_views_[slot_]) delayed_buffers_.emplace_back(swaps_, std::shared_ptr<void>(std::move(object)));
    if (retire_mask_ & 8) for (auto& object : retired_buffers_[slot_]) delayed_buffers_.emplace_back(swaps_, std::shared_ptr<void>(std::move(object)));
  }
  retired_framebuffer_slots_[slot_].clear();
  retired_slots_[slot_].clear();
  retired_views_[slot_].clear();
  retired_buffers_[slot_].clear();
  patch_out_used_ = 0;
  while (!delayed_buffers_.empty() && swaps_ - delayed_buffers_.front().first >= retire_delay_) {
    delayed_buffers_.pop_front();
  }
  if (swaps_ - last_eviction_ >= (low_memory_ ? 8u : 30u)) {
    last_eviction_ = swaps_;
    EvictIdleTextures();
  }
  StartList();
  // Only here, never from Flush: a mid-frame flush keeps recording into the
  // same range, and resetting it would drop what the first submission wrote.
  pass_marks_[slot_].clear();
  timestamps_used_ = 0;
  if (timestamp_pool_ != VK_NULL_HANDLE) {
    vkCmdResetQueryPool(static_cast<VulkanCommandList*>(L())->vk, timestamp_pool_,
                        slot_ * kMaxTimestamps, kMaxTimestamps);
    WriteTimestamp();
  }
  pair_capture_ = false;
  pair_used_ = 0;
  pair_marks_[slot_].clear();
  if (pair_pool_ != VK_NULL_HANDLE && pairprof_frames_left_ > 0) {
    vkCmdResetQueryPool(static_cast<VulkanCommandList*>(L())->vk, pair_pool_, slot_ * kPairQueries,
                        kPairQueries);
    pair_capture_ = true;
    --pairprof_frames_left_;
  }
}

// Resets the per-frame state that lives in the ring slice and opens the list.
void PlumeCommandProcessor::StartList() {
  SyncRecorder();
  upload_used_ = slice_base_;
  uploaded_streams_.clear();
  ++upload_generation_;
  for (auto& stage : constant_offsets_) {
    for (uint64_t& offset : stage) offset = UINT64_MAX;
  }
  shared_offset_ = UINT64_MAX;
  shared_cold_offset_ = UINT64_MAX;
  vs_all_offset_ = UINT64_MAX;
  // A new command list starts with no push constants of its own.
  pushed_constants_[0] = pushed_constants_[1] = pushed_constants_[2] = UINT64_MAX;
  std::fill(std::begin(bound_constant_offsets_), std::end(bound_constant_offsets_), UINT32_MAX);
  viewport_bound_ = scissor_bound_ = false;
  bound_vertex_buffers_.fill(VertexBinding{});
  bound_index_buffer_ = nullptr;
  bound_index_format_ = RenderFormat::UNKNOWN;
  L()->begin();
  list_open_ = true;
  bound_framebuffer_ = nullptr;
  bound_pipeline_ = nullptr;
  state_bound_ = false;
  // Two frames share the render targets, and a target whose layout does not
  // change gets no barrier of its own, so nothing would otherwise stop this
  // frame's writes from reaching the GPU alongside the last one's. One barrier
  // at the top orders this submission after everything already queued. It costs
  // GPU/GPU overlap, which is not what the pipelining is for - the win is this
  // list being recorded while the previous one runs.
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(static_cast<VulkanCommandList*>(L())->vk,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                       &barrier, 0, nullptr, 0, nullptr);
}

// Runs what was recorded so far and waits, to make room in the upload ring.
// A full drain: the slice cannot be rewritten while any frame still reads it.
void PlumeCommandProcessor::Flush() {
  FlushDeferred();
  if (!list_open_) return;
  L()->end();
  list_open_ = false;
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForAllSlots();
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  queue_->waitForCommandFence(fence_.get());
  StartList();
}

void PlumeCommandProcessor::Transition(RenderTexture* texture, RenderTextureLayout& current,
                                       RenderTextureLayout layout) {
  if (current == layout) return;
  RenderBarrierStages stages = (layout == RenderTextureLayout::COPY_SOURCE ||
                                layout == RenderTextureLayout::COPY_DEST)
                                   ? RenderBarrierStage::COPY
                                   : RenderBarrierStage::GRAPHICS;
  L()->barriers(stages, RenderTextureBarrier(texture, layout));
  ++stats_.barriers;
  current = layout;
  // A barrier ends the render pass, so every layout change made while one is
  // bound splits it in two. Counting them by destination says which is worth
  // chasing: the frame runs 82 passes, and one carrying a single draw costs
  // about 0.3 ms of GPU whatever it draws.
  if (bound_framebuffer_) {
    ++stats_.split_by[uint32_t(layout) & 7];
    // On a tiler the pass's fragment work runs at vkCmdEndRenderPass, which
    // this barrier just triggered - so the mark belongs here, after it, for the
    // interval since the pass opened to be that pass's own cost.
    PassMark mark;
    mark.resolve = 1;
    pass_marks_[slot_].push_back(mark);
    WriteTimestamp();
  }
  bound_framebuffer_ = nullptr;  // barriers end the render pass
}



uint32_t PlumeCommandProcessor::SwizzledResolved(Resolved& r, bool fitted, uint32_t swizzle, bool stencil_first) {
  RenderTexture* texture = fitted ? r.fitted.get() : r.texture.get();
  if (!texture) return 0;
  // Only a resolve that kept its stencil (RG32F) reads differently as 8_8_8_8.
  stencil_first = stencil_first && r.format == RenderFormat::R32G32_FLOAT;
  Resolved::SwizzledView& s = r.swizzled[swizzle | (uint32_t(fitted) << 12) | (uint32_t(stencil_first) << 13)];
  if (s.texture != texture || !s.view || !s.descriptor) {
    if (s.view) retired_views_[slot_].push_back(std::move(s.view));
    if (s.descriptor) retired_descriptor_slots_[slot_].push_back(s.descriptor);
    // Xenos swizzle lanes: 0..3 pick a channel, every one of them the
    // depth; 4 is 0, 5 is 1. Fetched as 8_8_8_8, a k_24_8 surface has its
    // stencil in the first channel (the low byte), kept in G.
    auto lane = [stencil_first](uint32_t code) {
      if (code == 0 && stencil_first) return RenderSwizzle::G;
      return code < 4 ? RenderSwizzle::R : code == 5 ? RenderSwizzle::ONE : RenderSwizzle::ZERO;
    };
    // An aliased depth map is read through its own format (the view takes the
    // depth aspect); its fitted copy is the R32_FLOAT a depth resolve makes.
    RenderTextureViewDesc desc =
        RenderTextureViewDesc::Texture2D(fitted && r.alias ? RenderFormat::R32_FLOAT : r.format);
    desc.componentMapping = RenderComponentMapping(lane(swizzle & 7), lane((swizzle >> 3) & 7),
                                                   lane((swizzle >> 6) & 7), lane((swizzle >> 9) & 7));
    s.view = texture->createTextureView(desc);
    s.descriptor = AllocateTextureDescriptor();
    s.texture = texture;
    if (!s.view || !s.descriptor) return 0;
    SetTexture2D(s.descriptor, texture, s.view.get());
  }
  return s.descriptor;
}

uint32_t PlumeCommandProcessor::FittedResolved(Resolved& r, uint32_t width, uint32_t height) {
  if (!r.fitted || r.fitted_width != width || r.fitted_height != height) {
    RetireSwizzled(r);
    RetireTexture(r.fitted, r.fitted_descriptor);
    r.fitted = device_->createTexture(
        RenderTextureDesc::Texture2D(width, height, 1, r.alias ? RenderFormat::R32_FLOAT : r.format));
    r.fitted_width = width;
    r.fitted_height = height;
    r.fitted_layout = RenderTextureLayout::UNKNOWN;
    r.fitted_generation = UINT64_MAX;
    r.fitted_descriptor = AllocateTextureDescriptor();
    if (!r.fitted || !r.fitted_descriptor) return 0;
    SetTexture2D(r.fitted_descriptor, r.fitted.get());
  }
  if (r.fitted_generation != r.generation) {
    if (r.alias) {
      // The depth image goes through CopyDepth, which takes it and its layout
      // from a target.
      Target image;
      image.texture = std::move(r.texture);
      image.layout = r.layout;
      image.depth = true;
      image.width = r.width;
      image.height = r.height;
      Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::COPY_DEST);
      CopyDepth(image, 0, 0, width, height, r.fitted.get(), 0, 0, false);
      r.texture = std::move(image.texture);
      r.layout = image.layout;
    } else {
      Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
      Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::COPY_DEST);
      RenderBox box(0, 0, int32_t(width), int32_t(height), 0, 1);
      L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.fitted.get()),
                               RenderTextureCopyLocation::Subresource(r.texture.get()), 0, 0, 0, &box);
    }
    r.fitted_generation = r.generation;
  }
  Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::SHADER_READ);
  return r.fitted_descriptor;
}



// ---------------------------------------------------------------------------
// Render targets per Direct3D surface (native lists). The surface object and
// its header name the target, so two surfaces sharing EDRAM are two targets,
// and its size is the surface's own (D3DSurface_GetDesc: +36 holds width - 1
// from bit 18 and height - 1 from bit 3; the width is the pitch, as for the
// EDRAM targets).

Target* PlumeCommandProcessor::NativeTarget(uint32_t slot, uint32_t info_register) {
  const ListSurface& s = native_surfaces_[slot];
  if (!s.object) {
    ++surface_missing_;
    return nullptr;
  }
  if (s.info != info_register) ++surface_mismatch_;
  return SurfaceTarget(slot);
}

Target* PlumeCommandProcessor::SurfaceTarget(uint32_t slot) {
  const ListSurface& s = native_surfaces_[slot];
  if (!s.object) return nullptr;
  const bool depth = slot == 4;
  const uint32_t format = depth ? (s.info >> 16) & 1 : (s.info >> 16) & 0xF;
  const uint32_t header = s.surface_info * 0x9E3779B1u ^ s.info * 0x85EBCA77u ^ s.dims * 0xC2B2AE3Du ^
                          uint32_t(depth);
  const uint64_t key = kSurfaceTargetKey | (uint64_t(header & 0x7FFFFFFFu) << 32) | s.object;
  Target& target = targets_[key];
  target.last_used = ++target_serial_;
  target.used_frame = swaps_;
  if (target.texture) return &target;
  const uint32_t pitch = s.surface_info & 0x3FFF;
  uint32_t width = std::max({pitch, ((s.dims >> 18) & 0x3FFF) + 1, 16u});
  uint32_t height = std::max(((((s.dims >> 3) & 0x7FFF) + 1) + 15) & ~15u, 16u);
  // A D24S8 shadow map is its D3D surface's size, not the EDRAM pitch padded to
  // whole tiles (1040 for 1024): the extra columns are never drawn, and sampled
  // as a resolved texture in place (AliasDepthResolve) they would stretch the map.
  if (depth && format == 0) {
    const uint32_t exact = std::max(((s.dims >> 18) & 0x3FFF) + 1, 16u);
    if (exact < width) width = exact;
  }
  // The dynamic shadow maps are the only D24S8 depth surfaces (the scene's is
  // D24FS8) of this size: drawn at the shadow quality's fraction of it.
  uint32_t scale = 4;
  if (depth && format == 0 && shadow_scale_ != 4 && std::min(width, height) >= 640) {
    scale = shadow_scale_;
  } else if (internal_scale_ != 4 && width == 1280 && height >= 704 && height <= 736) {
    // The scene's colour and depth: the internal resolution option.
    scale = internal_scale_;
  }
  if (scale != 4) {
    width = std::max((width * scale + 3) / 4, 16u);
    height = std::max((height * scale + 3) / 4, 16u);
  }
  const RenderFormat rf = depth ? RenderFormat::D32_FLOAT_S8_UINT
                                : ColorFormat(xenos::ColorRenderTargetFormat(format));
  target.texture = device_->createTexture(depth ? RenderTextureDesc::DepthTarget(width, height, rf)
                                                : RenderTextureDesc::ColorTarget(width, height, rf));
  if (!target.texture) {
    targets_.erase(key);
    return nullptr;
  }
  target.format = rf;
  target.width = width;
  target.height = height;
  target.depth = depth;
  target.scale = scale;
  target.layout = RenderTextureLayout::UNKNOWN;
  target.surface = s.object;
  target.surface_info = s.info;
  ++surface_created_;
  RetireFramebuffers();
  return &target;
}

void PlumeCommandProcessor::RetireFramebuffers() {
  SyncRecorder();
  for (auto& [key, framebuffer] : framebuffers_)
    retired_framebuffer_slots_[slot_].push_back(std::move(framebuffer));
  framebuffers_.clear();
  ++object_epoch_;
  bound_framebuffer_ = nullptr;
}

bool PlumeCommandProcessor::GrowSurfaceTarget(Target& target, uint32_t width, uint32_t height) {
  width = std::max(width, target.width);
  height = std::max(height, target.height);
  if (width == target.width && height == target.height) return true;
  auto texture = device_->createTexture(target.depth ? RenderTextureDesc::DepthTarget(width, height, target.format)
                                                     : RenderTextureDesc::ColorTarget(width, height, target.format));
  if (!texture) return false;
  if (target.texture) retired_slots_[slot_].push_back(std::move(target.texture));
  target.texture = std::move(texture);
  target.width = width;
  target.height = height;
  target.layout = RenderTextureLayout::UNKNOWN;
  target.restore_key = 0;  // what it held was lost with its image
  target.restore_image = nullptr;
  target.alias_cycles = 0;
  target.aliased_since_clear = false;
  ++surface_grown_;
  RetireFramebuffers();
  return true;
}

void PlumeCommandProcessor::MatchSurfaceSizes(std::array<Target*, 4>& colors, uint32_t color_count, Target*& depth) {
  Target* first = color_count ? colors[0] : depth;
  if (!first) return;
  uint32_t width = first->width, height = first->height;
  for (uint32_t i = 1; i < color_count; ++i) {
    if (colors[i]) width = std::max(width, colors[i]->width), height = std::max(height, colors[i]->height);
  }
  if (depth) width = std::max(width, depth->width), height = std::max(height, depth->height);
  for (uint32_t i = 0; i < color_count; ++i) {
    if (colors[i] && !GrowSurfaceTarget(*colors[i], width, height)) colors[i] = nullptr;
  }
  if (depth && !GrowSurfaceTarget(*depth, width, height)) depth = nullptr;
}

// Targets of surfaces not drawn to for ten seconds go (a released surface's
// object is reused for another).
void PlumeCommandProcessor::EvictSurfaceTargets() {
  if (swaps_ % 60) return;
  bool evicted = false;
  for (auto it = targets_.begin(); it != targets_.end();) {
    if ((it->first & kSurfaceTargetKey) && it->second.used_frame + 600 < swaps_) {
      if (it->second.texture) retired_slots_[slot_].push_back(std::move(it->second.texture));
      it = targets_.erase(it);
      ++surface_evicted_;
      evicted = true;
    } else {
      ++it;
    }
  }
  if (evicted) RetireFramebuffers();
}

static const char* Pm4OpcodeName(uint32_t op) {
  switch (op) {
    case xenos::PM4_NOP: return "NOP";
    case xenos::PM4_ME_INIT: return "ME_INIT";
    case xenos::PM4_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
    case xenos::PM4_INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
    case xenos::PM4_WAIT_REG_MEM: return "WAIT_REG_MEM";
    case xenos::PM4_WAIT_FOR_IDLE: return "WAIT_FOR_IDLE";
    case xenos::PM4_REG_RMW: return "REG_RMW";
    case xenos::PM4_MEM_WRITE: return "MEM_WRITE";
    case xenos::PM4_COND_WRITE: return "COND_WRITE";
    case xenos::PM4_COND_EXEC: return "COND_EXEC";
    case xenos::PM4_EVENT_WRITE: return "EVENT_WRITE";
    case xenos::PM4_EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
    case xenos::PM4_EVENT_WRITE_EXT: return "EVENT_WRITE_EXT";
    case xenos::PM4_EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
    case xenos::PM4_DRAW_INDX: return "DRAW_INDX";
    case xenos::PM4_DRAW_INDX_2: return "DRAW_INDX_2";
    case xenos::PM4_SET_CONSTANT: return "SET_CONSTANT";
    case xenos::PM4_SET_CONSTANT2: return "SET_CONSTANT2";
    case xenos::PM4_SET_SHADER_CONSTANTS: return "SET_SHADER_CONSTANTS";
    case xenos::PM4_LOAD_ALU_CONSTANT: return "LOAD_ALU_CONSTANT";
    case xenos::PM4_IM_LOAD: return "IM_LOAD";
    case xenos::PM4_IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
    case xenos::PM4_INVALIDATE_STATE: return "INVALIDATE_STATE";
    case xenos::PM4_INTERRUPT: return "INTERRUPT";
    case xenos::PM4_XE_SWAP: return "XE_SWAP";
    case xenos::PM4_VIZ_QUERY: return "VIZ_QUERY";
    case xenos::PM4_SET_BIN_MASK_LO:
    case xenos::PM4_SET_BIN_MASK_HI:
    case xenos::PM4_SET_BIN_SELECT_LO:
    case xenos::PM4_SET_BIN_SELECT_HI: return "SET_BIN_*";
    default: return "";
  }
}

std::string PlumeCommandProcessor::SurfaceReport() {
  size_t count = 0;
  for (const auto& [key, t] : targets_) count += (key & kSurfaceTargetKey) ? 1 : 0;
  std::string out = fmt::format(
      "== surfaces: {} targets now, {} created, {} grown, {} evicted; register/surface mismatches {}, "
      "slots without a surface {}\n== resolves: {} into their texture object, {} by address\n"
      "== executor: fences {}, presents {}, callbacks {}, "
      "vblank waits {} ({} ms, {} timed out); streams kept by buffer {}, by window {}\n",
      count, surface_created_, surface_grown_, surface_evicted_, surface_mismatch_, surface_missing_,
      object_resolves_, address_resolves_count_,
      native_fences_, native_presents_, native_callbacks_, native_waits_, native_wait_us_ / 1000,
      native_wait_timeouts_, object_streams_, window_streams_);
  for (const auto& [key, t] : targets_) {
    if (!(key & kSurfaceTargetKey)) continue;
    out += fmt::format("   surface {:08X} {} {}x{} base {} format {} last frame {}\n", t.surface,
                       t.depth ? "depth" : "color", t.width, t.height, t.surface_info & 0xFFF,
                       t.depth ? (t.surface_info >> 16) & 1 : (t.surface_info >> 16) & 0xF, t.used_frame);
  }
  return out;
}

RenderFramebuffer* PlumeCommandProcessor::GetFramebuffer(const std::array<Target*, 4>& colors,
                                                         uint32_t color_count, Target* depth) {
  // What SetFramebuffer labels the next pass with.
  color_count_of_pass_ = color_count;
  depth_of_pass_ = depth != nullptr;
  // The same attachments as the draw before: the same framebuffer, without
  // hashing them or asking the map.
  const RenderTexture* images[5] = {};
  for (uint32_t i = 0; i < color_count; ++i) images[i] = colors[i]->texture.get();
  images[4] = depth ? depth->texture.get() : nullptr;
  if (objkeys_ && last_framebuffer_.framebuffer && last_framebuffer_.epoch == object_epoch_ &&
      last_framebuffer_.color_count == color_count &&
      std::memcmp(last_framebuffer_.images, images, sizeof(images)) == 0) {
    return last_framebuffer_.framebuffer;
  }
  uint64_t key = 0;
  for (uint32_t i = 0; i < color_count; ++i) HashCombine(key, colors[i]->texture.get());
  HashCombine(key, depth ? depth->texture.get() : nullptr);
  HashCombine(key, color_count);
  auto& framebuffer = framebuffers_[key];
  if (!framebuffer) {
    const RenderTexture* attachments[4] = {};
    for (uint32_t i = 0; i < color_count; ++i) attachments[i] = colors[i]->texture.get();
    framebuffer = device_->createFramebuffer(
        RenderFramebufferDesc(attachments, color_count, depth ? depth->texture.get() : nullptr));
  }
  std::memcpy(last_framebuffer_.images, images, sizeof(images));
  last_framebuffer_.color_count = color_count;
  last_framebuffer_.epoch = object_epoch_;
  last_framebuffer_.framebuffer = framebuffer.get();
  return framebuffer.get();
}

void PlumeCommandProcessor::SetFramebuffer(RenderFramebuffer* framebuffer) {
  const bool recorder = OnRecorder();
  if (!recorder) SyncRecorder();
  if (!state_bound_) {
    L()->setGraphicsPipelineLayout(layout_.get());
    L()->setGraphicsDescriptorSet(set_2d_.get(), 0);
    L()->setGraphicsDescriptorSet(set_3d_.get(), 1);
    L()->setGraphicsDescriptorSet(set_cube_.get(), 2);
    L()->setGraphicsDescriptorSet(set_samplers_.get(), 3);
    state_bound_ = true;
  }
  if (bound_framebuffer_ != framebuffer) {
    // setFramebuffer ends the pass that was open, and the next one starts at
    // the first draw, so the mark lands between the two.
    L()->setFramebuffer(framebuffer);
    bound_framebuffer_ = framebuffer;
    ++rec_stats_.passes;
    PassMark mark;
    mark.width = framebuffer->getWidth();
    mark.height = framebuffer->getHeight();
    mark.colors = recorder ? emit_colors_ : color_count_of_pass_;
    mark.depth = recorder ? emit_depth_ : depth_of_pass_;
    pass_marks_[slot_].push_back(mark);
    WriteTimestamp();
  }
}

Resolved& PlumeCommandProcessor::GetResolved(uint32_t address, RenderFormat format, uint32_t width,
                                             uint32_t height) {
  InvalidateResolvedLookups();
  Resolved& resolved = resolved_[ResolvedKey(address, format)];
  if (resolved.texture && resolved.format == format && resolved.width >= width &&
      resolved.height >= height) {
    return resolved;
  }
  uint32_t w = std::max(width, resolved.format == format ? resolved.width : 0u);
  uint32_t h = std::max(height, resolved.format == format ? resolved.height : 0u);
  RetireSwizzled(resolved);
  RetireTexture(resolved.texture, resolved.descriptor);
  resolved.texture = device_->createTexture(RenderTextureDesc::Texture2D(w, h, 1, format));
  resolved.format = format;
  resolved.width = w;
  resolved.height = h;
  resolved.layout = RenderTextureLayout::UNKNOWN;
  resolved.descriptor = AllocateTextureDescriptor();
  return resolved;
}

// A shadow map's resolve without its copy. CopyDepth stages the depth through a
// buffer into R32_FLOAT, which is the same bits a depth image gives a fetch when
// it is sampled through a depth view, so the surface's image can simply become
// the resolved texture. The surface takes the image the previous map of this
// destination was in (the two swap every frame), or a new one the first time.
//
// The surface's contents are not there for the game any more. That is only
// seen if the game uses the surface again without clearing all of it, and then
// RestoreAliasedDepth copies them back from the resolved image, which is still
// the map as it was resolved - what the old path's surface held all along.
Resolved* PlumeCommandProcessor::AliasDepthResolve(Target& source, uint32_t address) {
  // The surface has a copy of its own pending: give it back before the image
  // goes, or the resolve would take an undefined one.
  if (source.restore_key) RestoreAliasedDepth(source, false);
  const RenderFormat format = source.format;
  const uint64_t key = ResolvedKey(address, format);
  Resolved& r = resolved_[key];
  // The name the texture is fetched by comes first, so that nothing has moved
  // when there is none to be had.
  if (!r.descriptor) r.descriptor = AllocateTextureDescriptor();
  if (!r.descriptor) {
    if (!r.texture) resolved_.erase(key);
    return nullptr;
  }
  // Another surface's map is in this destination: that surface gets its
  // contents back before its image is reused.
  if (r.alias && r.texture) {
    for (auto& [target_key, other] : targets_) {
      if (&other == &source || other.restore_key != key || other.restore_image != r.texture.get()) continue;
      if (other.alias_cycles >= 2) {
        // Its map is resolved in place and it is cleared whole before it is
        // used again, twice over: nothing of what it held is read.
        other.restore_key = 0;
        other.restore_image = nullptr;
        other.depth_undefined = true;
        ++stats_.depth_abandoned;
      } else {
        RestoreAliasedDepth(other, false);
      }
    }
  }
  std::unique_ptr<RenderTexture> fresh;
  RenderTextureLayout fresh_layout = RenderTextureLayout::UNKNOWN;
  if (r.alias && r.texture && r.width == source.width && r.height == source.height) {
    fresh = std::move(r.texture);
    fresh_layout = r.layout;
  } else {
    fresh = device_->createTexture(RenderTextureDesc::DepthTarget(source.width, source.height, format));
    if (!fresh) {
      if (!r.texture) {
        if (r.descriptor) retired_descriptor_slots_[slot_].push_back(r.descriptor);
        resolved_.erase(key);
      }
      return nullptr;
    }
  }
  InvalidateResolvedLookups();
  RetireSwizzled(r);
  if (r.texture) {
    // An alias of another size: its descriptor stays, the image goes.
    RetireFramebuffers();
    retired_slots_[slot_].push_back(std::move(r.texture));
  }
  r.texture = std::move(source.texture);
  r.layout = source.layout;
  r.format = format;
  r.width = source.width;
  r.height = source.height;
  r.alias = true;
  source.texture = std::move(fresh);
  source.layout = fresh_layout;
  source.restore_key = key;
  source.restore_image = r.texture.get();
  source.aliased_since_clear = true;
  ++stats_.depth_aliased;
  return &r;
}

// What a depth surface that was resolved in place is used for next, for the
// pattern AliasDepthResolve waits for: a clear of all of it ends the cycle and
// counts, anything else breaks the run. `whole` is a clear that covered the
// surface; the caller restores or drops the contents itself.
void PlumeCommandProcessor::NoteAliasedUse(Target& target, bool whole_clear) {
  if (!target.aliased_since_clear) return;
  target.aliased_since_clear = false;
  target.alias_cycles = whole_clear ? target.alias_cycles + 1 : 0;
}

bool PlumeCommandProcessor::RestoreAliasedDepth(Target& target, bool full_clear) {
  const uint64_t key = target.restore_key;
  const RenderTexture* image = target.restore_image;
  target.restore_key = 0;
  target.restore_image = nullptr;
  if (!key) return false;
  if (full_clear) {
    ++stats_.depth_dropped;
    return true;
  }
  auto found = resolved_.find(key);
  if (found == resolved_.end() || !found->second.alias || found->second.texture.get() != image ||
      !target.texture || found->second.width != target.width || found->second.height != target.height) {
    // The map was evicted or resolved over by another surface: what the target
    // held is gone, as it is for a target that was grown.
    return false;
  }
  Resolved& r = found->second;
  Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
  Transition(target.texture.get(), target.layout, RenderTextureLayout::COPY_DEST);
  L()->copyTexture(target.texture.get(), r.texture.get());
  ++stats_.depth_restored;
  return false;
}

// A texture's content key. The game streams textures into reused memory and
// fills them over several frames, so the start alone says nothing about the
// rest: sample the first kilobyte and 64 slices spread over the whole span.
uint64_t ContentHash(const uint8_t* data, uint32_t span) {
  if (span <= 64 * 1024) return XXH3_64bits(data, span);
  uint64_t hash = XXH3_64bits(data, 1024);
  const uint32_t step = span / 64;
  for (uint32_t i = 1; i <= 64; ++i) {
    const uint32_t at = std::min(i * step, span) - 256;
    hash = XXH3_64bits_withSeed(data + at, 256, hash);
  }
  return hash;
}

// The layout of every level a fetch constant names, and the key the decoded
// texture is kept under. All of it is a pure function of the constant, and the
// draw path asked for it once per texture slot of every draw: the busiest frame
// of Showdown Town ran this arithmetic 169 thousand times, a quarter of the
// command processor's time, for a few hundred distinct textures.
// The layout a fetch constant describes. Asked for the same constant at the
// same slot draw after draw: the slot remembers the six words and what they
// came to, and only a different constant (or a bumped object_epoch_) goes on to
// hash them.
const PlumeCommandProcessor::FetchLayout* PlumeCommandProcessor::LayoutOf(
    const xenos::xe_gpu_texture_fetch_t& fetch) {
  static_assert(sizeof(fetch) == 6 * sizeof(uint32_t));
  if (!objkeys_ || fetch_slot_ < 0) return LayoutOfByContents(fetch);
  SlotObject& object = slot_objects_[uint32_t(fetch_slot_) & 31];
  const uint32_t* words = reinterpret_cast<const uint32_t*>(&fetch);
  if (object.epoch == object_epoch_ && std::memcmp(object.words, words, sizeof(object.words)) == 0) {
    return object.layout;
  }
  const FetchLayout* layout = LayoutOfByContents(fetch);
  std::memcpy(object.words, words, sizeof(object.words));
  object.epoch = object_epoch_;
  object.layout = layout;
  object.texture = nullptr;
  return layout;
}

const PlumeCommandProcessor::FetchLayout* PlumeCommandProcessor::LayoutOfByContents(
    const xenos::xe_gpu_texture_fetch_t& fetch) {
  // The memo key: six words mixed by multiply and fold. Six XXH3 calls for one
  // small input each were a visible slice of the command thread (this runs for
  // every texture slot of every draw whose constant differs from the slot's
  // last). It is kept in memory only (layout_memo_, fetch_layouts_), never
  // written anywhere, so it need not match HashCombine.
  uint64_t fetch_key = 0x9E3779B97F4A7C15ull;
  for (const uint32_t word : {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
                              fetch.dword_5}) {
    fetch_key = (fetch_key ^ word) * 0xFF51AFD7ED558CCDull;
    fetch_key ^= fetch_key >> 32;
  }
  if (const FetchLayout* known = layout_memo_.Find(fetch_key)) return known->usable ? known : nullptr;
  auto found = fetch_layouts_.find(fetch_key);
  if (found != fetch_layouts_.end()) {
    layout_memo_.Put(fetch_key, &found->second);
    return found->second.usable ? &found->second : nullptr;
  }

  FetchLayout& layout = fetch_layouts_[fetch_key];
  layout_memo_.Put(fetch_key, &layout);
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);  // pitch, tiling, (not clamp/sign)
  HashCombine(key, fetch.dword_1);            // format, endian, base address
  HashCombine(key, fetch.dword_2);            // size
  HashCombine(key, fetch.swizzle);
  HashCombine(key, uint32_t(fetch.mip_min_level));
  HashCombine(key, uint32_t(fetch.mip_address));
  uint32_t base_page, mip_page, top_level, last_level;
  gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
      &base_page, &mip_page, &top_level, &last_level);
  if (skip_mask_ & 32768) { base_page = fetch.base_address; top_level = last_level = 0; }
  if (skip_mask_ & 8192) last_level = top_level;
  layout.levels = last_level - top_level + 1;
  layout.top_level = top_level;
  HashCombine(key, last_level);
  HashCombine(key, uint32_t(fetch.packed_mips));
  // The same memory read with and without mips is two host images under
  // texture quality, so the choice is part of the key.
  layout.base_map = fetch.mip_filter == xenos::TextureFilter::kBaseMap;
  HashCombine(key, uint32_t(layout.base_map));
  layout.key = key;
  for (uint32_t i = 0; i < layout.levels; ++i) {
    auto& level = layout.levels_layout[i];
    if (!LocateTextureLevel(fetch, top_level + i, level)) return nullptr;
    const uint64_t start = (uint64_t(level.from_mips ? mip_page : base_page) << 12) + level.offset;
    if (!start || start + level.extent > 0x20000000ull) return nullptr;
    layout.starts[i] = uint32_t(start);
  }
  layout.usable = true;
  return &layout;
}

uint32_t PlumeCommandProcessor::GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address) return 0;
  const FetchLayout* fetch_layout = LayoutOf(fetch);
  if (!fetch_layout) return 0;
  const uint64_t key = fetch_layout->key;
  const uint32_t levels = fetch_layout->levels;
  const std::array<TextureLevel, 14>& layouts = fetch_layout->levels_layout;
  std::array<const uint8_t*, 14> data{};
  uint64_t content = 0;
  for (uint32_t i = 0; i < levels; ++i) {
    data[i] = memory_->TranslatePhysical<const uint8_t*>(fetch_layout->starts[i]);
  }
  // The guest's top mips a low-memory host does not keep: a texture a side of
  // 2048 held as RGBA8 is 21 MB with its chain and 1.4 MB from 512 down, and
  // at this render size nothing shows the difference. Coordinates are
  // normalised, so the shaders sample the smaller image the same way.
  uint32_t skip = 0;
  if (max_texture_dim_) {
    while (skip + 1 < levels && std::max(layouts[skip].width, layouts[skip].height) > max_texture_dim_) ++skip;
  }
  // Texture quality: one mip lower for half, two for quarter, on a texture the
  // guest filters across mips, and never to a side under 64 px. What it reads
  // from its base level alone (the interface, fonts) is not touched.
  if (texture_quality_ && !fetch_layout->base_map) {
    uint32_t reduced = 0;
    while (reduced < texture_quality_ && reduced + 1 < levels &&
           std::max(layouts[reduced + 1].width, layouts[reduced + 1].height) >= 64) {
      ++reduced;
    }
    skip = std::max(skip, reduced);
  }
  const uint32_t host_levels = levels - skip;
  // The slot's own record of the texture its constant resolved to last draw
  // (see SlotObject), checked against the layout it was made for.
  SlotObject* slot_object = objkeys_ && fetch_slot_ >= 0 ? &slot_objects_[uint32_t(fetch_slot_) & 31] : nullptr;
  if (slot_object && (slot_object->epoch != object_epoch_ || slot_object->layout != fetch_layout)) {
    slot_object = nullptr;
  }
  GuestTexture* texture_node = slot_object ? slot_object->texture : nullptr;
  if (!texture_node) {
    texture_node = texture_memo_.Find(key);
    if (!texture_node) {
      texture_node = &guest_textures_[key];
      texture_memo_.Put(key, texture_node);
    }
    if (slot_object) slot_object->texture = texture_node;
  }
  GuestTexture& texture = *texture_node;
  if (texture.failed) return 0;
  // The block format this texture is kept in: its own when the device samples
  // it and the blocks fit, RGBA8 decoded on the CPU otherwise (and always while
  // a texture dump asks for the decoded pixels).
  const BcKind want = bc_supported_ && bc_mode_ && !texture.bc_blocked && texdump_directory_.empty()
                          ? BcKindFor(fetch.format)
                          : BcKind::kNone;
  // Checked once per frame; the draws after the first reuse the result. A
  // texture whose memory is write-watched and was not written since is not
  // checked at all - hashing every texture in use every frame was a tenth of
  // this thread - except once every 256 frames, in case a write went around
  // the watch.
  DrainWatchHits();
  const bool unchanged =
      texture.checked_frame == swaps_ || (texture.watched && ((swaps_ + key) & 255) != 0);
  if (texture.texture && unchanged && texdump_directory_.empty() && texture.bc == want && texture.skip == skip) {
    texture.checked_frame = swaps_;
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  if (keep_streams_ && !texture.watched) {
    // Watch first, hash second, so that a write landing in between is seen.
    texture.watch_begin = {UINT32_MAX, UINT32_MAX};
    texture.watch_end = {0, 0};
    for (uint32_t i = 0; i < levels; ++i) {
      const uint32_t which = layouts[i].from_mips ? 1 : 0;
      texture.watch_begin[which] = std::min(texture.watch_begin[which], fetch_layout->starts[i]);
      texture.watch_end[which] =
          std::max(texture.watch_end[which], fetch_layout->starts[i] + layouts[i].extent);
    }
    for (uint32_t which = 0; which < 2; ++which) {
      if (texture.watch_begin[which] >= texture.watch_end[which]) {
        texture.watch_begin[which] = texture.watch_end[which] = 0;
        continue;
      }
      memory_->EnablePhysicalMemoryAccessCallbacks(texture.watch_begin[which],
                                                   texture.watch_end[which] - texture.watch_begin[which],
                                                   true, false);
      IndexBlocks(texture_block_index_, texture.watch_begin[which],
                  texture.watch_end[which] - texture.watch_begin[which], key);
    }
    texture.watched = true;
  }
  // The mips the host does not keep (see skip) are not read, so not hashed.
  for (uint32_t i = skip; i < levels; ++i) HashCombine(content, ContentHash(data[i], layouts[i].extent));
  if (!texdump_directory_.empty() && dumped_.insert(key).second) texture.content = ~content;
  if (texture.texture && texture.content == content && texture.bc == want && texture.skip == skip) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> rgba;
  std::vector<uint8_t> blocks;
  const uint32_t width = layouts[skip].width, height = layouts[skip].height;
  BcKind kind = want;
  if (kind != BcKind::kNone && !CopyTextureLevelBlocks(fetch, layouts[skip], data[skip], blocks)) {
    // Not a shape a block upload takes: this texture goes through the decoder.
    texture.bc_blocked = true;
    kind = BcKind::kNone;
  }
  if (kind == BcKind::kNone && !DecodeTextureLevel(fetch, layouts[skip], data[skip], 0, rgba)) {
    texture.failed = true;
    ++stats_.texture_failures;
    if (unsupported_formats_.insert(uint32_t(fetch.format)).second) {
      REXLOG_WARN("Renut: Plume does not decode texture format {} yet", uint32_t(fetch.format));
    }
    return 0;
  }
  if (!texdump_directory_.empty()) DumpTexture(fetch, rgba, width, height, "2d");
  RenderFormat host_format = RenderFormat::R8G8B8A8_UNORM;
  switch (kind) {
    case BcKind::kBC1: host_format = RenderFormat::BC1_UNORM; break;
    case BcKind::kBC2: host_format = RenderFormat::BC2_UNORM; break;
    case BcKind::kBC3: host_format = RenderFormat::BC3_UNORM; break;
    case BcKind::kBC4: host_format = RenderFormat::BC4_UNORM; break;
    case BcKind::kBC5: host_format = RenderFormat::BC5_UNORM; break;
    default: break;
  }
  // New contents reuse the image when its shape is unchanged: earlier draws of
  // this frame may still reference it, and the upload is ordered after them.
  if (!texture.texture || texture.levels != host_levels || texture.width != width || texture.height != height ||
      texture.bc != kind) {
    RetireGuestTexture(texture);
    texture.texture = device_->createTexture(RenderTextureDesc::Texture2D(width, height, host_levels, host_format));
    texture.levels = host_levels;
    texture.width = width;
    texture.height = height;
    texture.bc = kind;
    texture.skip = skip;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
    if (kind != BcKind::kNone) {
      // The decoder applied the fetch constant's swizzle to the pixels; the
      // blocks keep the guest's channels, so the view applies it. Identity
      // (X Y Z W, the usual one) needs no view.
      uint32_t swizzle[4];
      bool identity = true;
      for (int i = 0; i < 4; ++i) {
        swizzle[i] = (fetch.swizzle >> (3 * i)) & 7;
        identity = identity && swizzle[i] == uint32_t(i);
      }
      if (!identity) {
        auto pick = [](uint32_t s) {
          switch (s) {
            case 0: return RenderSwizzle::R;
            case 1: return RenderSwizzle::G;
            case 2: return RenderSwizzle::B;
            case 3: return RenderSwizzle::A;
            case 5: return RenderSwizzle::ONE;
            default: return RenderSwizzle::ZERO;
          }
        };
        RenderTextureViewDesc view = RenderTextureViewDesc::Texture2D(host_format);
        view.componentMapping = RenderComponentMapping(pick(swizzle[0]), pick(swizzle[1]), pick(swizzle[2]),
                                                       pick(swizzle[3]));
        texture.view = texture.texture->createTextureView(view);
      }
    }
  }
  texture.content = content;
  if (!texture.descriptor) texture.descriptor = AllocateTextureDescriptor();
  if (!texture.descriptor) {
    // Out of descriptors: try again next frame, once eviction has freed some,
    // rather than keep this texture white for as long as it stays in use.
    texture.content = ~content;
    return 0;
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  uint32_t w = width, h = height;
  for (uint32_t level = 0; level < host_levels; ++level) {
    w = layouts[skip + level].width;
    h = layouts[skip + level].height;
    if (kind != BcKind::kNone) {
      // The guest's blocks as they are; a level that does not fit its memory
      // (not seen) stays zero blocks rather than leave the image half written.
      if (level && !CopyTextureLevelBlocks(fetch, layouts[skip + level], data[skip + level], blocks)) {
        blocks.assign(size_t((w + 3) / 4) * ((h + 3) / 4) * BcBlockBytes(kind), 0);
      }
      const uint64_t staging = Allocate(blocks.size(), 512);
      if (staging == UINT64_MAX) return 0;
      std::memcpy(Pointer(staging), blocks.data(), blocks.size());
      // The row is counted in texels and a multiple of the 4-texel block.
      L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), level),
                               RenderTextureCopyLocation::PlacedFootprint(
                                   upload_.get(), host_format, w, h, 1, ((w + 3) / 4) * 4, staging));
      continue;
    }
    if (level && !DecodeTextureLevel(fetch, layouts[skip + level], data[skip + level], 0, rgba)) return 0;
    const uint64_t staging = Allocate(uint64_t(w) * h * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), rgba.data(), size_t(w) * h * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), level),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, w, h, 1, w, staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  SetTexture2D(texture.descriptor, texture.texture.get(), texture.view.get());
  ++stats_.textures_decoded;
  if (kind != BcKind::kNone) ++stats_.textures_bc;
  return texture.descriptor;
}

void PlumeCommandProcessor::DumpTexture(const xenos::xe_gpu_texture_fetch_t& fetch,
                                        const std::vector<uint32_t>& rgba, uint32_t width,
                                        uint32_t height, const char* kind) {
  static uint32_t serial = 0;
  const auto path = fmt::format(
      "{}/{:04}_{}_{:08X}_f{}_{}x{}_t{}_e{}_p{}_sw{:03X}_s{}{}{}{}_n{}_m{}_mn{}_ma{:08X}_pk{}.rgba", texdump_directory_, serial++,
      kind, fetch.base_address << 12, uint32_t(fetch.format), width, height, uint32_t(fetch.tiled),
      uint32_t(fetch.endianness), uint32_t(fetch.pitch), uint32_t(fetch.swizzle), uint32_t(fetch.sign_x),
      uint32_t(fetch.sign_y), uint32_t(fetch.sign_z), uint32_t(fetch.sign_w), uint32_t(fetch.num_format),
      uint32_t(fetch.mip_max_level), uint32_t(fetch.mip_min_level), fetch.mip_address << 12,
      uint32_t(fetch.packed_mips));
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(rgba.data(), 4, size_t(width) * height, file);
    std::fclose(file);
  }
}

// The sampler a fetch constant asks for. The guest states every axis on its
// own - magnification, minification and mip filters, anisotropy, a clamp mode
// per axis and a LOD bias - and Plume used to reduce all of that to "point or
// linear" and the X axis's wrap mode, which cost the textures their sharpness
// at grazing angles and addressed Y and Z with a mode meant for X.
//
// One exception: a stacked texture's Z picks a slice of an array, and the guest
// clamps a layer to the ends of the stack whatever the mode reads.
uint32_t PlumeCommandProcessor::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t key = uint32_t(fetch.mag_filter) | (uint32_t(fetch.min_filter) << 2) |
                       (uint32_t(fetch.mip_filter) << 4) | (uint32_t(fetch.aniso_filter) << 6) |
                       (uint32_t(fetch.clamp_x) << 9) | (uint32_t(fetch.clamp_y) << 12) |
                       (uint32_t(fetch.clamp_z) << 15) | (uint32_t(fetch.stacked) << 18) |
                       ((uint32_t(fetch.lod_bias) & 0x3FF) << 19);
  // The fallback is the old behaviour: linear everywhere, no anisotropy, and
  // every axis wrapped, for comparing a capture against.
  if (skip_mask_ & 524288) return kSamplerDefault;
  if (const uint32_t* memo = sampler_memo_.Find(key)) return *memo;
  auto found = sampler_indices_.find(key);
  if (found != sampler_indices_.end()) {
    sampler_memo_.Put(key, &found->second);
    return found->second;
  }
  if (next_sampler_ >= kMaxSamplers) return kSamplerDefault;

  auto filter = [](xenos::TextureFilter f) {
    return f == xenos::TextureFilter::kPoint ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  };
  auto address = [](xenos::ClampMode clamp) {
    switch (clamp) {
      case xenos::ClampMode::kRepeat: return RenderTextureAddressMode::WRAP;
      case xenos::ClampMode::kMirroredRepeat: return RenderTextureAddressMode::MIRROR;
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway:
        return RenderTextureAddressMode::MIRROR_ONCE;
      case xenos::ClampMode::kClampToBorder:
      case xenos::ClampMode::kMirrorClampToBorder:
        return RenderTextureAddressMode::BORDER;
      default: return RenderTextureAddressMode::CLAMP;
    }
  };

  RenderSamplerDesc desc;
  desc.magFilter = filter(fetch.mag_filter);
  desc.minFilter = filter(fetch.min_filter);
  // kBaseMap is the guest asking for the top level alone, not for a mip filter.
  desc.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear
                        ? RenderMipmapMode::LINEAR
                        : RenderMipmapMode::NEAREST;
  if (fetch.mip_filter == xenos::TextureFilter::kBaseMap) desc.maxLOD = 0.0f;
  desc.addressU = address(fetch.clamp_x);
  desc.addressV = address(fetch.clamp_y);
  desc.addressW = fetch.stacked ? RenderTextureAddressMode::CLAMP : address(fetch.clamp_z);
  // The bias has five fractional bits.
  desc.mipLODBias = float(fetch.lod_bias) / 32.0f;
  if (fetch.aniso_filter != xenos::AnisoFilter::kDisabled &&
      fetch.aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (uint32_t(fetch.aniso_filter) - 1);
  }
  if (xenos::ClampModeUsesBorder(fetch.clamp_x) || xenos::ClampModeUsesBorder(fetch.clamp_y) ||
      xenos::ClampModeUsesBorder(fetch.clamp_z)) {
    desc.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White
                           ? RenderBorderColor::OPAQUE_WHITE
                           : RenderBorderColor::TRANSPARENT_BLACK;
  }

  std::unique_ptr<RenderSampler> sampler = device_->createSampler(desc);
  if (!sampler) return kSamplerDefault;
  const uint32_t index = next_sampler_++;
  samplers_[index] = std::move(sampler);
  set_samplers_->setSampler(index, samplers_[index].get());
  sampler_indices_.emplace(key, index);
  return index;
}

// Stacked textures (arrays of 2D slices: the terrain's material and normal map
// sets, and the fur bump map on Banjo, whose 16 slices are one relief at
// growing amplitude), which the shaders sample with tfetch3D, z picking the
// slice. Slices follow each other at a 4 KB aligned stride, bound as a 3D
// texture of that depth.
//
// A host 3D image halves its depth at every mip level, which would blend one
// slice into the next, so only a stack one slice deep can carry the guest's mip
// chain; a deeper stack keeps the single level it had before, as every stack in
// the menu does.
uint32_t PlumeCommandProcessor::GetGuestStackedTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address || (skip_mask_ & 65536)) return 0;
  const uint32_t depth = uint32_t(fetch.size_2d.stack_depth) + 1;
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);
  HashCombine(key, fetch.dword_1);
  HashCombine(key, fetch.dword_2);
  HashCombine(key, fetch.swizzle);
  HashCombine(key, uint32_t(fetch.mip_min_level));
  HashCombine(key, uint32_t(fetch.mip_address));
  uint32_t base_page, mip_page, top_level, last_level;
  gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
      &base_page, &mip_page, &top_level, &last_level);
  if (skip_mask_ & 32768) { base_page = fetch.base_address; top_level = last_level = 0; }
  if (depth > 1 || (skip_mask_ & (8192 | 262144))) last_level = top_level;
  const uint32_t levels = last_level - top_level + 1;
  HashCombine(key, last_level);
  HashCombine(key, uint32_t(fetch.packed_mips));
  std::array<TextureLevel, 14> layouts;
  std::array<const uint8_t*, 14> data{};
  for (uint32_t i = 0; i < levels; ++i) {
    TextureLevel& level = layouts[i];
    if (!LocateTextureLevel(fetch, top_level + i, level)) return 0;
    const uint64_t start = (uint64_t(level.from_mips ? mip_page : base_page) << 12) + level.offset;
    if (!start || start + level.extent > 0x20000000ull) return 0;
    data[i] = memory_->TranslatePhysical<const uint8_t*>(uint32_t(start));
  }
  GuestTexture& texture = guest_stacks_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_ && texdump_directory_.empty()) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  uint64_t content = 0;
  for (uint32_t i = 0; i < levels; ++i) HashCombine(content, ContentHash(data[i], layouts[i].extent));
  if (!texdump_directory_.empty() && dumped_.insert(key ^ 2).second) texture.content = ~content;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  // Every level is decoded before the image is touched: a level that fails
  // partway has to leave the contents an earlier frame uploaded alone.
  std::array<std::vector<uint32_t>, 14> all;
  std::vector<uint32_t> slice;
  for (uint32_t i = 0; i < levels; ++i) {
    for (uint32_t z = 0; z < depth; ++z) {
      if (!DecodeTextureLevel(fetch, layouts[i], data[i], z, slice)) {
        texture.failed = true;
        ++stats_.texture_failures;
        return 0;
      }
      if (!texdump_directory_.empty()) {
        DumpTexture(fetch, slice, layouts[i].width, layouts[i].height, "stack");
      }
      all[i].insert(all[i].end(), slice.begin(), slice.end());
    }
  }
  const uint32_t width = layouts[0].width, height = layouts[0].height;
  if (!texture.descriptor) {
    texture.descriptor = Allocate3DDescriptor();
    if (!texture.descriptor) return 0;
  }
  if (!texture.texture || texture.width != width || texture.height != height ||
      texture.depth != depth || texture.levels != levels) {
    if (texture.texture) retired_slots_[slot_].push_back(std::move(texture.texture));
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture3D(width, height, depth, levels, RenderFormat::R8G8B8A8_UNORM));
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.levels = levels;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
  }
  texture.content = content;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < levels; ++i) {
    const uint32_t w = layouts[i].width, h = layouts[i].height;
    const uint64_t staging = Allocate(uint64_t(w) * h * depth * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), all[i].data(), all[i].size() * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), i),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, w, h, depth, w,
                                 staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_3d_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_.textures_decoded;
  return texture.descriptor;
}

// Volume textures (dimension k3D). The particle simulations draw their wind
// and turbulence from one: D38D10B70F954F86 turns a texel of it into the push
// every falling leaf gets each frame. Unbound, the slot read the white
// fallback, the same (1, 1, 1) push for every leaf, and the leaves streamed
// off along one diagonal into sheets across the screen. Only the base level:
// the vertex shaders that sample these do so at level 0, and a host 3D image
// would halve its depth at each mip anyway.
uint32_t PlumeCommandProcessor::GetGuestVolumeTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address) return 0;
  std::vector<uint32_t> rgba;
  uint32_t width = 0, height = 0, depth = 0, extent = 0;
  if (!DecodeTexture3D(fetch, nullptr, rgba, width, height, depth, extent) ||
      uint64_t(address) + extent > 0x20000000ull) {
    return 0;
  }
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);
  HashCombine(key, fetch.dword_1);
  HashCombine(key, fetch.dword_2);
  HashCombine(key, fetch.swizzle);
  const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(address);
  GuestTexture& texture = guest_volumes_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  const uint64_t content = ContentHash(data, extent);
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  if (!DecodeTexture3D(fetch, data, rgba, width, height, depth, extent)) {
    texture.failed = true;
    ++stats_.texture_failures;
    REXLOG_WARN("Renut: Plume could not decode volume texture {:08X} f{} {}x{}x{}", address,
                uint32_t(fetch.format), width, height, depth);
    return 0;
  }
  if (!texture.descriptor) {
    texture.descriptor = Allocate3DDescriptor();
    if (!texture.descriptor) return 0;
  }
  if (!texture.texture || texture.width != width || texture.height != height || texture.depth != depth) {
    if (texture.texture) retired_slots_[slot_].push_back(std::move(texture.texture));
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture3D(width, height, depth, 1, RenderFormat::R8G8B8A8_UNORM));
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.levels = 1;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
    REXLOG_INFO("Renut: Plume volume texture {:08X} f{} {}x{}x{} tiled={}", address, uint32_t(fetch.format),
                width, height, depth, uint32_t(fetch.tiled));
  }
  texture.content = content;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  const uint64_t staging = Allocate(uint64_t(rgba.size()) * 4, 512);
  if (staging == UINT64_MAX) return 0;
  std::memcpy(Pointer(staging), rgba.data(), rgba.size() * 4);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), 0),
                           RenderTextureCopyLocation::PlacedFootprint(
                               upload_.get(), RenderFormat::R8G8B8A8_UNORM, width, height, depth, width,
                               staging));
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_3d_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_.textures_decoded;
  return texture.descriptor;
}

// Cube maps (the sky dome): six 2D faces, +X -X +Y -Y +Z -Z, one after the
// other at a 4 KB aligned stride, each decoded like a 2D texture.
uint32_t PlumeCommandProcessor::GetGuestCubeTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address || (skip_mask_ & 16384)) return 0;
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);
  HashCombine(key, fetch.dword_1);
  HashCombine(key, fetch.dword_2);
  HashCombine(key, fetch.swizzle);
  const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(address);
  const uint32_t face_stride = (TextureBaseSpan(fetch) + 4095) & ~4095u;
  GuestTexture& texture = guest_cubes_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_ && texdump_directory_.empty()) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  const uint64_t content = ContentHash(data, face_stride * 6);
  if (!texdump_directory_.empty() && dumped_.insert(key ^ 1).second) texture.content = ~content;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> faces[6];
  uint32_t width = 0, height = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    uint32_t w = 0, h = 0;
    if (!face_stride || !DecodeTexture2D(fetch, data + size_t(face) * face_stride, faces[face], w, h) ||
        (face && (w != width || h != height)) || w != h) {
      texture.failed = true;
      ++stats_.texture_failures;
      return 0;
    }
    width = w;
    height = h;
  }
  if (!texdump_directory_.empty()) {
    for (uint32_t face = 0; face < 6; ++face) DumpTexture(fetch, faces[face], width, height, "cube");
  }
  if (!texture.descriptor) {
    texture.descriptor = AllocateCubeDescriptor();
    if (!texture.descriptor) return 0;
  }
  if (!texture.texture) {
    texture.texture = device_->createTexture(RenderTextureDesc::Texture(
        RenderTextureDimension::TEXTURE_2D, width, height, 1, 1, 6, RenderFormat::R8G8B8A8_UNORM,
        RenderTextureFlag::CUBE));
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
    RenderTextureViewDesc view;
    view.format = RenderFormat::R8G8B8A8_UNORM;
    view.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
    texture.view = texture.texture->createTextureView(view);
    texture.layout = RenderTextureLayout::UNKNOWN;
  }
  texture.content = content;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  for (uint32_t face = 0; face < 6; ++face) {
    const uint64_t staging = Allocate(uint64_t(width) * height * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), faces[face].data(), size_t(width) * height * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), 0, face),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, width,
                                 staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_cube_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ,
                        texture.view.get());
  ++stats_.textures_decoded;
  return texture.descriptor;
}

void PlumeCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    if (register_file_->values[index] != value) {
      register_file_->values[index] = value;
      MarkConstantsWritten(index, 1);
    }
    return;
  }
  if (index >= gfx::RegisterFile::kRegisterCount) {
    extended_register_values_.insert_or_assign(index, value);
    return;
  }
  gfx::RegisterFile& regs = *register_file_;
  // Volatile: the waits in the lists' sync read these from other threads.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    // Scratch register write-back: what a callback's waits and the swap's
    // pacing read in memory.
    const uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1u << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      StoreBig(regs.values[XE_GPU_REG_SCRATCH_ADDR] + scratch_reg * 4, value);
    }
  } else if (index == XE_GPU_REG_COHER_STATUS_HOST) {
    const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
  }
}

void PlumeCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      uint64_t(start_index) + num_registers <= uint64_t(XE_GPU_REG_SHADER_CONSTANT_LOOP_31) + 1) {
    CopySwapped(reinterpret_cast<uint8_t*>(&register_file_->values[start_index]),
                reinterpret_cast<const uint8_t*>(base), size_t(num_registers) * sizeof(uint32_t),
                xenos::Endian::k8in32);
    MarkConstantsWritten(start_index, num_registers);
    return;
  }
  for (uint32_t i = 0; i < num_registers; ++i) {
    WriteRegister(start_index + i, rex::memory::load_and_swap<uint32_t>(base + i));
  }
}

gfx::Shader* PlumeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                               const uint32_t* host_address, uint32_t dword_count) {
  // Across frames: an address and length are the shader last found there while
  // the guest has not written under them (see ShaderWatch). Looked up first,
  // so a shader the game binds every frame is never hashed again.
  ShaderWatch* watch = nullptr;
  uint64_t watch_key = 0;
  if (shader_cache_ && guest_address && dword_count && dword_count <= (64u << 10)) {
    DrainWatchHits();
    watch_key = (uint64_t(guest_address) << 32) | dword_count;
    watch = shader_watch_memo_.Find(watch_key);
    if (!watch) {
      auto found = shader_watch_.find(watch_key);
      if (found != shader_watch_.end()) {
        watch = &found->second;
        shader_watch_memo_.Put(watch_key, watch);
      }
    }
    if (watch && watch->valid) {
      ++shader_cache_hits_;
      return watch->shader;
    }
  }
  // Watch first, hash second: a write that lands during the hash is a hit,
  // drained at the next lookup, and drops the entry made below.
  bool watched = false;
  if (watch_key) {
    if (!watch) {
      watch = &shader_watch_[watch_key];
      shader_watch_memo_.Put(watch_key, watch);
    }
    const uint32_t bytes = dword_count * 4;
    const bool dynamic = watch->drops >= 3 && swaps_ - watch->dropped_frame < 900;
    if (!dynamic && memory_->GetPhysicalHeap()->QueryRangeAccess(guest_address, guest_address + bytes - 1) ==
                        rex::memory::PageAccess::kReadWrite) {
      memory_->EnablePhysicalMemoryAccessCallbacks(guest_address, bytes, true, false);
      watched = true;
    }
  }
  gfx::Shader* result = LoadShaderByContents(shader_type, host_address, dword_count);
  if (watched && result) {
    watch->shader = static_cast<HostShader*>(result);
    watch->base = guest_address;
    watch->bytes = dword_count * 4;
    watch->valid = true;
    IndexBlocks(shader_block_index_, guest_address, dword_count * 4, watch_key);
  }
  return result;
}

gfx::Shader* PlumeCommandProcessor::LoadShaderByContents(xenos::ShaderType shader_type,
                                                         const uint32_t* host_address, uint32_t dword_count) {
  // The guest binds the same few hundred shaders thousands of times a frame,
  // and hashing the whole microcode each time was a steady few percent of
  // this thread. Within a frame an address and length stand for the shader
  // found there first: microcode a draw of this frame may still run is not
  // rewritten under it.
  if (shader_lookup_frame_ != swaps_) {
    shader_lookup_.clear();
    shader_memo_.Clear();
    shader_lookup_frame_ = swaps_;
  }
  const uint64_t memo_key = uint64_t(reinterpret_cast<uintptr_t>(host_address)) ^ (uint64_t(dword_count) << 48);
  if (HostShader* memo = shader_memo_.Find(memo_key)) return memo;
  auto known = shader_lookup_.find(host_address);
  if (known != shader_lookup_.end() && known->second.first == dword_count) {
    shader_memo_.Put(memo_key, known->second.second);
    return known->second.second;
  }
  uint64_t hash = XXH3_64bits(host_address, size_t(dword_count) * sizeof(uint32_t));
  auto& shader = shaders_[hash];
  if (!shader) {
    shader = std::make_unique<HostShader>(shader_type, hash, host_address, dword_count);
    const size_t ucode_bytes = size_t(dword_count) * sizeof(uint32_t);
    shader->ucode_key = ucode::Key(host_address, ucode_bytes);
    shader->loose_key = ucode::LooseKey(host_address, ucode_bytes);
    shader->entry = store_->Find(shader->ucode_key, shader->loose_key);
    if (!shader->entry && missed_.insert(hash).second && missed_.size() <= 32) {
      REXLOG_WARN("Renut: Plume has no recompiled {} shader for microcode {:016X} ({} dwords)",
                  shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, dword_count);
      if (!miss_dump_directory_.empty()) {
        auto path = fmt::format("{}/{}_{:016X}.ucode", miss_dump_directory_,
                                shader_type == xenos::ShaderType::kVertex ? "vs" : "ps", hash);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fwrite(host_address, 4, dword_count, file);
          std::fclose(file);
        }
      }
    }
  }
  shader_lookup_[host_address] = {dword_count, shader.get()};
  shader_memo_.Put(memo_key, shader.get());
  return shader.get();
}

// Debug (RENUT_PIPELINE_STATS in plume-env): what the driver's compiler made of
// each stage - instructions, registers, preamble and so on - one line per
// executable, to see what a draw of the guest's shaders costs.
void LogPipelineStatistics(RenderPipeline& pipeline, const HostShader* vs, const HostShader* ps) {
  auto* vulkan = static_cast<VulkanGraphicsPipeline*>(&pipeline);
  VkDevice device = vulkan->device->vk;
  if (!vkGetPipelineExecutablePropertiesKHR || !vkGetPipelineExecutableStatisticsKHR) return;
  VkPipelineInfoKHR info = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
  info.pipeline = vulkan->vk;
  uint32_t count = 0;
  if (vkGetPipelineExecutablePropertiesKHR(device, &info, &count, nullptr) != VK_SUCCESS) return;
  std::vector<VkPipelineExecutablePropertiesKHR> executables(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
  vkGetPipelineExecutablePropertiesKHR(device, &info, &count, executables.data());
  for (uint32_t i = 0; i < count; ++i) {
    VkPipelineExecutableInfoKHR executable = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    executable.pipeline = vulkan->vk;
    executable.executableIndex = i;
    uint32_t stat_count = 0;
    vkGetPipelineExecutableStatisticsKHR(device, &executable, &stat_count, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> stats(stat_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
    vkGetPipelineExecutableStatisticsKHR(device, &executable, &stat_count, stats.data());
    std::string line = fmt::format("Renut: Plume stats vs={:016X} ps={:016X} {}:", vs && vs->entry ? vs->entry->hash : 0,
                                   ps && ps->entry ? ps->entry->hash : 0, executables[i].name);
    for (const auto& stat : stats) {
      switch (stat.format) {
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: line += fmt::format(" [{}]={}", stat.name, stat.value.b32); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: line += fmt::format(" [{}]={}", stat.name, stat.value.i64); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: line += fmt::format(" [{}]={}", stat.name, stat.value.u64); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: line += fmt::format(" [{}]={}", stat.name, stat.value.f64); break;
        default: break;
      }
    }
    REXLOG_INFO("{}", line);
  }
}

// Debug (RENUT_PIPELINE_IR in plume-env, "all" or a list of shader hashes):
// the driver's internal representations of each stage - final NIR and the
// ir3 assembly with its preamble - written to <dir>/ir/<vs>_<ps>.txt.
void DumpPipelineInternals(RenderPipeline& pipeline, const HostShader* vs, const HostShader* ps,
                           const std::string& dir) {
  const char* filter = getenv("RENUT_PIPELINE_IR");
  if (!filter || dir.empty() || !vkGetPipelineExecutableInternalRepresentationsKHR) return;
  const uint64_t vs_hash = vs && vs->entry ? vs->entry->hash : 0;
  const uint64_t ps_hash = ps && ps->entry ? ps->entry->hash : 0;
  if (std::strcmp(filter, "all") != 0 && !std::strstr(filter, fmt::format("{:016X}", vs_hash).c_str()) &&
      !std::strstr(filter, fmt::format("{:016X}", ps_hash).c_str())) {
    return;
  }
  auto* vulkan = static_cast<VulkanGraphicsPipeline*>(&pipeline);
  VkDevice device = vulkan->device->vk;
  VkPipelineInfoKHR info = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
  info.pipeline = vulkan->vk;
  uint32_t count = 0;
  if (vkGetPipelineExecutablePropertiesKHR(device, &info, &count, nullptr) != VK_SUCCESS) return;
  std::vector<VkPipelineExecutablePropertiesKHR> executables(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
  vkGetPipelineExecutablePropertiesKHR(device, &info, &count, executables.data());
  renutMakeDirectory(dir + "/ir");
  FILE* file = std::fopen(fmt::format("{}/ir/{:016X}_{:016X}.txt", dir, vs_hash, ps_hash).c_str(), "w");
  if (!file) return;
  for (uint32_t i = 0; i < count; ++i) {
    VkPipelineExecutableInfoKHR executable = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    executable.pipeline = vulkan->vk;
    executable.executableIndex = i;
    uint32_t ir_count = 0;
    vkGetPipelineExecutableInternalRepresentationsKHR(device, &executable, &ir_count, nullptr);
    std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(
        ir_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
    vkGetPipelineExecutableInternalRepresentationsKHR(device, &executable, &ir_count, irs.data());
    std::vector<std::vector<char>> text(ir_count);
    for (uint32_t j = 0; j < ir_count; ++j) {
      text[j].resize(irs[j].dataSize + 1, 0);
      irs[j].pData = text[j].data();
    }
    vkGetPipelineExecutableInternalRepresentationsKHR(device, &executable, &ir_count, irs.data());
    for (uint32_t j = 0; j < ir_count; ++j) {
      std::fprintf(file, "==== %s: %s ====\n%s\n", executables[i].name, irs[j].name, text[j].data());
    }
  }
  std::fclose(file);
}

bool PlumeCommandProcessor::Drop(const char* reason, const HostShader* vs, const HostShader* ps) {
  const uint64_t vs_hash = vs && vs->entry ? vs->entry->hash : 0;
  const uint64_t ps_hash = ps && ps->entry ? ps->entry->hash : 0;
  const bool exports = vs && vs->entry && (vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT);
  ++drops_[{reason, vs_hash, ps_hash, exports}];
  Trace("dropped ({}) vs={:016X} ps={:016X}{}", reason, vs_hash, ps_hash, exports ? " memexport" : "");
  return true;
}

// ---------------------------------------------------------------------------
// Native Direct3D lists (see the declarations).

void PlumeCommandProcessor::NativeSwapIn() {
  if (native_swapped_) return;
  if (!native_regs_) {
    // The device's initial state came as PM4 at start-up: begin from it.
    native_regs_ = std::make_unique<gfx::RegisterFile>();
    std::memcpy(native_regs_->values, register_file_->values, sizeof(native_regs_->values));
  }
  ring_regs_ = register_file_;
  register_file_ = native_regs_.get();
  ring_vs_ = active_vertex_shader_;
  ring_ps_ = active_pixel_shader_;
  active_vertex_shader_ = native_vs_;
  active_pixel_shader_ = native_ps_;
  ring_bin_mask_ = bin_mask_;
  ring_bin_select_ = bin_select_;
  bin_mask_ = native_bin_mask_;
  bin_select_ = native_bin_select_;
  // The constant blocks are compared with what was last uploaded only when
  // marked written; another register file may hold anything.
  for (auto& stage : constants_written_) std::fill(std::begin(stage), std::end(stage), true);
  loop_constants_dirty_ = true;
  native_swapped_ = true;
}

void PlumeCommandProcessor::NativeSwapOut() {
  if (!native_swapped_) return;
  native_vs_ = active_vertex_shader_;
  native_ps_ = active_pixel_shader_;
  native_bin_mask_ = bin_mask_;
  native_bin_select_ = bin_select_;
  register_file_ = ring_regs_;
  active_vertex_shader_ = ring_vs_;
  active_pixel_shader_ = ring_ps_;
  bin_mask_ = ring_bin_mask_;
  bin_select_ = ring_bin_select_;
  for (auto& stage : constants_written_) std::fill(std::begin(stage), std::end(stage), true);
  loop_constants_dirty_ = true;
  native_swapped_ = false;
}

void PlumeCommandProcessor::NativeWrite(uint32_t first, const uint32_t* values, uint32_t count) {
  if (first >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      uint64_t(first) + count <= uint64_t(XE_GPU_REG_SHADER_CONSTANT_LOOP_31) + 1) {
    StoreConstants(first, values, count);
    return;
  }
  for (uint32_t i = 0; i < count; ++i) WriteRegister(first + i, values[i]);
}

// A draw record, as the command processor runs a DRAW_INDX packet.
void PlumeCommandProcessor::NativeDraw(uint32_t flags, uint32_t initiator, uint32_t dma_base, uint32_t dma_size) {
  if ((flags & native_d3d::kDrawPredicated) &&
      !((bin_select_ & bin_mask_) != 0 || (bin_mask_ != 0 && renut::d3d::GetSingleBinTiling())) &&
      !OnPredicatedSkip(xenos::PM4_DRAW_INDX)) {
    return;
  }
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
  reg::VGT_DRAW_INITIATOR draw;
  draw.value = initiator;
  IndexBufferInfo info;
  bool indexed = false;
  if (draw.source_select == xenos::SourceSelect::kDMA) {
    WriteRegister(XE_GPU_REG_VGT_DMA_BASE, dma_base);
    WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, dma_size);
    reg::VGT_DMA_SIZE size;
    size.value = dma_size;
    const uint32_t bytes = draw.index_size == xenos::IndexFormat::kInt16 ? 2 : 4;
    info.guest_base = dma_base & ~(bytes - 1);
    info.endianness = size.swap_mode;
    info.format = draw.index_size;
    info.length = size.num_words * bytes;
    info.count = draw.num_indices;
    indexed = true;
  } else if (draw.source_select != xenos::SourceSelect::kAutoIndex) {
    ++native_bad_;
    return;
  }
  const auto viz = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
  if (viz.viz_query_ena && viz.kill_pix_post_hi_z) return;
  ++native_draws_;
  IssueDraw(draw.prim_type, draw.num_indices, indexed ? &info : nullptr,
            xenos::IsMajorModeExplicit(draw.major_mode, draw.prim_type));
}

bool PlumeCommandProcessor::RunNative(uint32_t until) {
  bool found = false, swapped_here = false;
  for (;;) {
    if (native_stack_.empty()) {
      if (native_queue_.empty()) {
        std::vector<std::shared_ptr<const native_d3d::List>> more;
        native_d3d::TakePublished(more);
        for (auto& list : more) native_queue_.push_back(std::move(list));
        if (native_queue_.empty()) break;
      }
      native_stack_.push_back({std::move(native_queue_.front()), 0});
      native_queue_.pop_front();
    }
    NativeFrame& f = native_stack_.back();
    const std::vector<uint32_t>& words = f.list->words;
    if (f.pos >= words.size()) {
      native_stack_.pop_back();
      if (native_stack_.empty()) native_d3d::NoteListExecuted();
      continue;
    }
    const uint32_t* w = words.data() + f.pos;
    const uint32_t op = w[0] & 0xFF;
    if (!native_swapped_) {
      NativeSwapIn();
      swapped_here = true;
    }
    switch (op) {
      case native_d3d::kOpRegs: {
        const uint32_t count = w[0] >> 8, first = w[1];
        if (f.pos + 2 + count > words.size() || first + count > gfx::RegisterFile::kRegisterCount) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        NativeWrite(first, w + 2, count);
        f.pos += 2 + count;
        break;
      }
      case native_d3d::kOpShader: {
        const auto type = xenos::ShaderType(w[1] & 3);
        const uint32_t address = w[1] & ~3u, dwords = w[2];
        gfx::Shader* shader = LoadShader(type, address, memory_->TranslatePhysical<const uint32_t*>(address), dwords);
        if (type == xenos::ShaderType::kVertex) {
          active_vertex_shader_ = shader;
        } else {
          active_pixel_shader_ = shader;
        }
        f.pos += 5;
        break;
      }
      case native_d3d::kOpDraw:
        f.pos += 6;
        NativeDraw(w[0] >> 8, w[2], w[3], w[4]);
        break;
      case native_d3d::kOpBin: {
        const uint32_t which = w[0] >> 8, value = w[1];
        if (which == 0) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
        if (which == 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(value) << 32);
        if (which == 2) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
        if (which == 3) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(value) << 32);
        f.pos += 2;
        break;
      }
      case native_d3d::kOpRun: {
        std::shared_ptr<const native_d3d::List> child = w[1] < f.list->runs.size() ? f.list->runs[w[1]] : nullptr;
        f.pos += 2;
        if (child) native_stack_.push_back({std::move(child), 0});
        break;
      }
      case native_d3d::kOpSwap:
        f.pos += 4;
        break;
      case native_d3d::kOpResolve:
        if (f.pos + native_d3d::kResolveWords > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += native_d3d::kResolveWords;
        NativeResolve(w);
        break;
      case native_d3d::kOpClear:
        if (f.pos + native_d3d::kClearWords > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += native_d3d::kClearWords;
        NativeClear(w);
        break;
      case native_d3d::kOpFence:
        if (f.pos + 4 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        // The ring position to +4, then the fence to +0, big-endian, as the
        // two EVENT_WRITE_SHDs land.
        StoreBig(w[1] + 4, w[2]);
        StoreBig(w[1], w[3]);
        ++native_fences_;
        f.pos += 4;
        break;
      case native_d3d::kOpPresent: {
        if (f.pos + 6 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += 6;
        // As the ring had it: a history slot stamped with the vblank counter,
        // the swap, the counter moving on, the second slot.
        NativeSwapOut();
        if (w[4]) StoreBig(w[4], counter());
        native_presenting_ = true;
        renut_note_guest_swap();
        IssueSwap(w[1], w[2], w[3]);
        native_presenting_ = false;
        increment_counter();
        if (w[5]) StoreBig(w[5], counter());
        ++native_presents_;
        if (native_presents_ <= 3 || native_presents_ % 600 == 0) {
          REXLOG_INFO("Renut: native present {} ({:08X} {}x{}); fences {}, callbacks {}",
                      native_presents_, w[1], w[2], w[3], native_fences_, native_callbacks_);
        }
        break;
      }
      case native_d3d::kOpEffects: {
        const uint32_t count = w[0] >> 8;
        if (f.pos + 1 + 2 * size_t(count) > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += 1 + 2 * size_t(count);
        NativeEffects(w + 1, count);
        break;
      }
      case native_d3d::kOpStreams: {
        const uint32_t mask = (w[0] >> 8) & 0x3FFFF;
        const size_t n = 1 + 2 * size_t(__builtin_popcount(mask));
        if (f.pos + n > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        const uint32_t* q = w + 1;
        for (uint32_t stream = 0; stream < 18; ++stream) {
          if (!((mask >> stream) & 1)) continue;
          native_streams_[stream] = {q[0], q[1]};
          q += 2;
        }
        f.pos += n;
        break;
      }
      case native_d3d::kOpWaitMemory: {
        if (f.pos + 4 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += 4;
        // As the GPU would, until the vertical-blank handler (another thread)
        // writes the value; never longer than a few frames.
        const auto* word = memory_->TranslatePhysical<const uint32_t*>(w[1]);
        const auto start = std::chrono::steady_clock::now();
        ++native_waits_;
        while ((__builtin_bswap32(__atomic_load_n(word, __ATOMIC_ACQUIRE)) & w[3]) != w[2]) {
          if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) {
            ++native_wait_timeouts_;
            break;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        native_wait_us_ += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
        break;
      }
      case native_d3d::kOpIndices:
        if (f.pos + 3 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        native_indices_ = {w[1], w[2]};
        f.pos += 3;
        break;
      case native_d3d::kOpSurfaces: {
        const uint32_t slots = (w[0] >> 8) & 0x1F;
        const size_t n = 1 + 4 * size_t(__builtin_popcount(slots));
        if (f.pos + n > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        const uint32_t* p = w + 1;
        for (uint32_t slot = 0; slot < 5; ++slot) {
          if (!((slots >> slot) & 1)) continue;
          native_surfaces_[slot] = {p[0], p[1], p[2], p[3]};
          p += 4;
        }
        f.pos += n;
        break;
      }
      default:
        ++native_bad_;
        f.pos = words.size();
        break;
    }
  }
  if (swapped_here) NativeSwapOut();
  return found;
}

// A record of register writes and interrupts, in order (what a callback does without its
// PM4 words, see native_d3d.cpp): the same effects NativeCallback takes from the packets.
void PlumeCommandProcessor::NativeEffects(const uint32_t* pairs, uint32_t count) {
  NativeSwapOut();
  ++native_callbacks_;
  ++native_effects_applied_;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t reg = pairs[2 * i], value = pairs[2 * i + 1];
    if (reg == 0xFFFFFFFFu) {
      if (!dispatch_interrupt_) continue;
      for (uint32_t cpu = 0; cpu < 6; ++cpu) {
        if ((value >> cpu) & 1) dispatch_interrupt_(1, cpu);
      }
    } else {
      WriteRegister(reg, value);
    }
  }
}

// A clear record: Direct3D's clear of the current targets, without its draw.
// The targets are the bound surfaces', and only the clear's rectangle is
// cleared.
void PlumeCommandProcessor::NativeClear(const uint32_t* w) {
  FlushDeferred();
  if (skip_mask_ & 134217728) return;
  const uint32_t flags = w[0] >> 8, present = w[17];
  const int32_t x1 = int32_t(w[1]), y1 = int32_t(w[2]), x2 = int32_t(w[3]), y2 = int32_t(w[4]);
  float color[4], z;
  std::memcpy(color, w + 5, sizeof(color));
  std::memcpy(&z, w + 9, sizeof(z));
  const uint32_t stencil = w[10] & 0xFF;
  ++stats_.clears;
  auto clip = [&](const Target& t) {
    const auto low = [&](int32_t v) { return int32_t(HostFloor(&t, uint32_t(std::max(v, 0)))); };
    const auto high = [&](int32_t v) { return int32_t(HostCeil(&t, uint32_t(std::max(v, 0)))); };
    return RenderRect(std::clamp(low(x1), 0, int32_t(t.width)), std::clamp(low(y1), 0, int32_t(t.height)),
                      std::clamp(high(x2), 0, int32_t(t.width)), std::clamp(high(y2), 0, int32_t(t.height)));
  };
  Trace("native clear flags={:X} present={:X} rect=({},{})-({},{}) color=({}, {}, {}, {}) z={} s={}", flags,
        present, x1, y1, x2, y2, color[0], color[1], color[2], color[3], z, stencil);
  std::array<Target*, 4> cleared{};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((flags >> i) & 1) || !((present >> i) & 1)) continue;
    Target* target = NativeTarget(i, w[12 + i]);
    if (!target || std::find(cleared.begin(), cleared.end(), target) != cleared.end()) continue;
    cleared[i] = target;
    const RenderRect rect = clip(*target);
    if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
    Transition(target->texture.get(), target->layout, RenderTextureLayout::COLOR_WRITE);
    std::array<Target*, 4> colors{target};
    SetFramebuffer(GetFramebuffer(colors, 1, nullptr));
    L()->clearColor(0, RenderColor(color[0], color[1], color[2], color[3]), &rect, 1);
  }
  const bool clear_depth = flags & 0x10, clear_stencil = flags & 0x20;
  if ((clear_depth || clear_stencil) && (present & 0x10)) {
    if (Target* depth = NativeTarget(4, w[16])) {
      const RenderRect rect = clip(*depth);
      if (rect.right > rect.left && rect.bottom > rect.top) {
        // A surface whose image went to a depth resolve (AliasDepthResolve): a
        // clear of the whole depth needs nothing of the old contents - the
        // stencil comes up undefined in the other image, so it is cleared too -
        // and a smaller one or a stencil-only clear needs them back.
        bool fresh = false;
        if (depth->restore_key || depth->aliased_since_clear || depth->depth_undefined) {
          const bool whole = clear_depth && rect.left <= 0 && rect.top <= 0 && rect.right >= int32_t(depth->width) &&
                             rect.bottom >= int32_t(depth->height);
          NoteAliasedUse(*depth, whole);
          if (depth->restore_key) fresh = RestoreAliasedDepth(*depth, whole);
          if (depth->depth_undefined) {
            // The contents were given up (AliasDepthResolve): the stencil is
            // undefined as well, which a whole clear settles.
            fresh = fresh || whole;
            depth->depth_undefined = !whole;
          }
        }
        Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
        std::array<Target*, 4> none{};
        SetFramebuffer(GetFramebuffer(none, 0, depth));
        L()->clearDepthStencil(clear_depth, clear_stencil || fresh, std::clamp(z, 0.f, 1.f), stencil, &rect, 1);
      }
    }
  }
}



bool PlumeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info, bool) {
  struct InDraw {
    bool& flag;
    explicit InDraw(bool& f) : flag(f) { flag = true; }
    ~InDraw() { flag = false; }
  } in_draw(in_issue_draw_);
  // Only the native lists draw: a draw packet in the ring draws nothing.
  if (!native_swapped_) {
    ++native_ring_draws_dropped_;
    return true;
  }
  const gfx::RegisterFile& regs = *register_file_;
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) return true;  // resolves are records
  ++stats_.draws;
  const uint32_t draw_index = frame_draw_++;
  if (draw_index >= drawskip_first_ && draw_index < drawskip_end_) return true;
  for (const ConstOverride& o : const_overrides_) {
    if (o.bool_word) {
      uint32_t& word = register_file_->values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + o.reg];
      word = (word | o.set) & ~o.clear;
      continue;
    }
    const uint32_t at = (o.pixel ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X) + o.reg * 4;
    std::memcpy(&register_file_->values[at], o.value, sizeof(o.value));
    MarkConstantsWritten(at, 4);
  }

  auto* vs = static_cast<HostShader*>(active_vertex_shader());
  if (trace_ && (!vs || !vs->entry)) {
    const auto dc = draw_util::GetNormalizedDepthControl(regs);
    Trace("dropped vs={:016X} prim={} n={} mode={} rt0={:X} ds={:X} z={}{}{} f{} colormask={:X}",
          vs ? vs->ucode_data_hash() : 0, uint32_t(prim_type), index_count, uint32_t(edram_mode),
          uint32_t(regs.Get<reg::RB_COLOR_INFO>().color_base),
          uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base), dc.z_enable ? "t" : "",
          dc.z_write_enable ? "w" : "", dc.stencil_enable ? "s" : "", uint32_t(dc.zfunc),
          regs[XE_GPU_REG_RB_COLOR_MASK]);
  }
  if (!vs || !vs->entry) {
    ++stats_.no_vs;
    if (vs) {
      auto* ps_miss = static_cast<HostShader*>(active_pixel_shader());
      const uint64_t ps_hash = ps_miss && ps_miss->entry ? ps_miss->entry->hash : 0;
      ++missed_draws_[vs->ucode_data_hash()];
      if (missed_pairs_.insert(vs->ucode_data_hash() ^ ps_hash).second && missed_pairs_.size() <= 48) {
        REXLOG_INFO("Renut: Plume missing VS {:016X} with PS {:016X}, mode {}, prim {}, {} indices",
                    vs->ucode_data_hash(), ps_hash, uint32_t(edram_mode), uint32_t(prim_type), index_count);
      }
    }
    return true;
  }
  auto* ps = static_cast<HostShader*>(active_pixel_shader());
  const bool color_pass = edram_mode == xenos::EdramMode::kColorDepth;
  if (color_pass && (!ps || !ps->entry)) {
    ++stats_.no_ps;
    return true;
  }
  if (!color_pass) ps = nullptr;
  if (!vs->compiled) vs->compiled = store_->Build(device_, vs->entry, true, true);
  if (ps && !ps->compiled) ps->compiled = store_->Build(device_, ps->entry, false);
  if (!vs->compiled || (ps && !ps->compiled)) {
    ++stats_.bad_shader;
    return true;
  }
  if (!vs->elements_decoded) DecodeLiveElements(*vs);

  // Primitive.
  RenderPrimitiveTopology topology = RenderPrimitiveTopology::UNKNOWN;
  bool point_list = false, rect_list = false, quad_list = false, patch_list = false;
  bool quad_patch = false;
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList:
      // Particles. Every point becomes a sprite in point_list.geom, sized by
      // what the recompiled vertex shader exports at the point-sprite location.
      topology = RenderPrimitiveTopology::POINT_LIST;
      point_list = true;
      break;
    case xenos::PrimitiveType::kLineList: topology = RenderPrimitiveTopology::LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = RenderPrimitiveTopology::LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleFan: topology = RenderPrimitiveTopology::TRIANGLE_FAN; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case xenos::PrimitiveType::kRectangleList:
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      rect_list = true;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      quad_list = true;
      break;
    case xenos::PrimitiveType::kQuadPatch:
      // The sea of the first world, as a grid of quads.
      quad_patch = true;
      [[fallthrough]];
    case xenos::PrimitiveType::kTrianglePatch:
      // The water. The guest's shader runs as the domain stage, once per point
      // of a patch that Plume expands itself: the driver here reports no
      // tessellation stage, and the expansion is the same triangles it would
      // have made.
      if (!vs->domain_decoded) {
        vs->domain_decoded = true;
        vs->domain_entry = store_->FindDomain(vs->ucode_key, vs->loose_key);
        if (vs->domain_entry) {
          vs->domain_compiled = store_->Build(device_, vs->domain_entry, false, true);
          std::vector<LiveElement> unused;
          DecodeLiveElements(*vs, vs->domain_entry, unused, vs->domain_live_fetches);
        }
      }
      if (!vs->domain_compiled) break;
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      patch_list = true;
      break;
    default:
      break;
  }
  if (topology == RenderPrimitiveTopology::UNKNOWN) {
    ++stats_.skipped_prim;
    if ((prim_type == xenos::PrimitiveType::kTrianglePatch || prim_type == xenos::PrimitiveType::kQuadPatch) &&
        patch_drop_logged_.insert(vs->entry->hash).second) {
      // Water with no domain form simply does not appear. tools/prepare.py dumps reads this line back.
      REXLOG_INFO(
          "Renut: Plume patch draw dropped: vs={:016X} has no domain form (list it in "
          "tools/shaders/domain-shaders.txt)",
          vs->entry->hash);
    }
    // The patches Plume could not tessellate go through here: the shader and
    // the tessellation mode say which.
    Trace("dropped prim={} n={} vs={:016X} ps={:016X} tess={} indexed={}", uint32_t(prim_type),
          index_count, vs->entry->hash, ps ? ps->entry->hash : 0,
          uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode),
          uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().source_select));
    return true;
  }
  if ((rect_list && !rect_list_gs_) || (point_list && !point_list_gs_)) {
    ++stats_.skipped_prim;
    return true;
  }

  // Hi-Z: a draw whose box lies wholly outside the frustum or the scissor puts
  // nothing on the target; it is dropped before its targets, textures, streams,
  // constants and pipeline cost anything. Exact (hiz_model.h). The resolves a
  // dropped draw would have fetched are fetched by the draws that remain: only
  // mesh draws (triangles through a vertex model) are ever dropped, never the
  // full-screen passes that read the scene's copies.
  hiz_test_.valid = false;
  if (hiz_mode_ && !trace_ && !skip_mask_ && !bind_skip_ && !sync_draws_ && !patch_list && !rect_list &&
      !point_list && vs->compiled && (!debug_shader_ || (vs->entry->hash != debug_shader_ &&
                                                          (!ps || ps->entry->hash != debug_shader_)))) {
    // One call in 16 is timed (the clock reads are a cost of their own).
    const bool timed = (++hiz_timer_phase_ & 15) == 0;
    hiz_stage_timing_ = timed;
    std::chrono::steady_clock::time_point cull_start;
    if (timed) cull_start = std::chrono::steady_clock::now();
    const bool dropped = HizCullDraw(*vs, prim_type, index_count, index_buffer_info, color_pass);
    if (timed) {
      stats_.hiz_cull_ns += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - cull_start).count());
    }
    if (dropped) {
      ++stats_.hiz_culled_total;
      return true;
    }
  }
  // Render targets.
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t pitch = surface.surface_pitch;
  if (!pitch) return Drop("no pitch", vs, ps);
  const uint32_t height = std::clamp<uint32_t>(regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y, 16, 4096);
  const uint32_t color_mask = regs.Get<reg::RB_COLOR_MASK>().value;
  static constexpr gfx::Register kColorInfo[4] = {
      XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO,
      XE_GPU_REG_RB_COLOR3_INFO};
  static constexpr gfx::Register kBlendControl[4] = {
      XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2,
      XE_GPU_REG_RB_BLENDCONTROL3};
  std::array<Target*, 4> colors{};
  uint32_t color_count = 0;
  if (color_pass) {
    for (uint32_t i = 0; i < 4; ++i) {
      if ((color_mask >> (4 * i)) & 0xF) color_count = i + 1;
    }
    for (uint32_t i = 0; i < color_count; ++i) {
      reg::RB_COLOR_INFO info;
      info.value = regs[kColorInfo[i]];
      colors[i] = NativeTarget(i, info.value);
      if (!colors[i]) {
        ++stats_.no_target;
        return true;
      }
    }
  }
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  Target* depth = nullptr;
  if (depth_control.z_enable || depth_control.stencil_enable) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    depth = NativeTarget(4, depth_info.value);
  }
  {
    MatchSurfaceSizes(colors, color_count, depth);
    for (uint32_t i = 0; i < color_count; ++i) {
      if (!colors[i]) {
        ++stats_.no_target;
        return true;
      }
    }
  }
  if (!color_count && !depth) return Drop("no color or depth", vs, ps);
  // Debug (plume-bind 16): drop depth-only draws - the shadow casters - to
  // price them.
  if ((bind_skip_ & 16) && !color_count) return true;
  // files/plume-shadowmin: a shadow caster with fewer indices than this casts
  // no shadow. Only depth-only draws that write depth into a target that is not
  // the screen's: a stencil mark, a memory export or the scene's own depth is
  // never dropped. Off (0) by default.
  if (shadow_min_indices_ && !color_count && index_count < shadow_min_indices_ && pitch != 1280 &&
      depth_control.z_write_enable && !depth_control.stencil_enable &&
      !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) {
    ++stats_.shadow_culled;
    return true;
  }
  // MatchSurfaceSizes may have recreated a texture that an earlier slot also uses.
  for (uint32_t i = 0; i < color_count; ++i) {
    Transition(colors[i]->texture.get(), colors[i]->layout, RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    // A surface whose image went to a depth resolve and is drawn to without
    // being cleared: what it held comes back (AliasDepthResolve).
    if (depth->restore_key) RestoreAliasedDepth(*depth, false);
    // Used again without a clear: not the pattern (and whatever it holds is what
    // it holds).
    if (depth->aliased_since_clear) NoteAliasedUse(*depth, false);
    depth->depth_undefined = false;
    Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
  }

  // Textures used by the draw: resolved render targets, by guest address.
  uint32_t texture_descriptor[32] = {};
  uint32_t cube_descriptor[32] = {};
  uint32_t stack_descriptor[32] = {};
  uint32_t sampler_descriptor[32] = {};
  // Only the slots the bound shaders actually sample. The game leaves the other
  // fetch constants set from earlier draws, and asking all 32 for a texture, a
  // sampler and a resolved target was most of what this path did.
  auto slots_of = [&](HostShader* shader) {
    if (!shader) return 0u;
    if (!shader->texture_mask_decoded) {
      shader->texture_mask = TextureMask(*shader);
      shader->texture_mask_decoded = true;
    }
    return shader->texture_mask;
  };
  uint32_t used_slots = slots_of(vs) | slots_of(ps);
  // Debug: go back to binding every slot, to compare against a capture.
  if (skip_mask_ & 1048576) used_slots = ~0u;
  for (uint32_t remaining = used_slots; remaining;) {
    const uint32_t slot = uint32_t(__builtin_ctz(remaining));
    remaining &= remaining - 1;
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
    if (fetch.type != xenos::FetchConstantType::kTexture) continue;
    sampler_descriptor[slot] = GetSampler(fetch);
    Resolved* found = FindResolved(fetch.base_address << 12);
    if (found && found->descriptor) {
      Resolved& r = *found;
      if (snap_shader_ && !snap_taken_ && !r.depth && r.texture && ps && ps->entry->hash == snap_shader_ &&
          !miss_dump_directory_.empty() &&
          access((miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/plume-snap-dump").c_str(),
                 F_OK) == 0) {
        SnapshotResolved(r);
        Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
      }
      if (depth_dump_ps_ && !depth_dump_ && r.depth && !r.alias && r.texture && ps &&
          ps->entry->hash == depth_dump_ps_) {
        const uint32_t bytes = r.format == RenderFormat::R32G32_FLOAT ? 8 : 4;
        depth_dump_row_ = (r.width + 63) & ~63u;
        depth_dump_ = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(depth_dump_row_) * r.height * bytes));
        if (depth_dump_) {
          Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
          CopyTextureToBuffer(L(), depth_dump_.get(), r.texture.get(), r.width, r.height, depth_dump_row_, 0, 0,
                              nullptr);
          Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
          depth_dump_w_ = r.width;
          depth_dump_h_ = r.height;
          depth_dump_format_ = uint32_t(r.format);
          depth_dump_generation_ = r.generation;
          depth_dump_frame_ = r.written_frame;
        }
      }
      const uint32_t fw = uint32_t((uint64_t(fetch.size_2d.width + 1) * r.scale + 3) / 4);
      const uint32_t fh = uint32_t((uint64_t(fetch.size_2d.height + 1) * r.scale + 3) / 4);
      // A depth resolve goes through a view that repeats the depth (see
      // Resolved::swizzled). A colour resolve already holds the channels in
      // the order its fetch swizzle expects; applying it again swapped red
      // and blue across the UI.
      const uint32_t swizzle = uint32_t(fetch.swizzle) & 0xFFF;
      const bool depth_view = r.depth;
      const bool stencil_first = fetch.format == xenos::TextureFormat::k_8_8_8_8;
      if (fw < r.width || fh < r.height) {
        texture_descriptor[slot] = FittedResolved(r, std::min(fw, r.width), std::min(fh, r.height));
        if (depth_view && texture_descriptor[slot]) texture_descriptor[slot] = SwizzledResolved(r, true, swizzle, stencil_first);
      } else {
        Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
        texture_descriptor[slot] = depth_view ? SwizzledResolved(r, false, swizzle, stencil_first) : r.descriptor;
      }
    } else if (fetch.dimension == xenos::DataDimension::k2DOrStacked) {
      fetch_slot_ = int(slot);
      texture_descriptor[slot] = GetGuestTexture(fetch);
      fetch_slot_ = -1;
      if (fetch.stacked) stack_descriptor[slot] = GetGuestStackedTexture(fetch);
    } else if (fetch.dimension == xenos::DataDimension::kCube) {
      cube_descriptor[slot] = GetGuestCubeTexture(fetch);
    } else if (fetch.dimension == xenos::DataDimension::k3D) {
      // tfetch3D reads the same table as a stack's.
      stack_descriptor[slot] = GetGuestVolumeTexture(fetch);
    }
  }

  if (skip_shader_ && (vs->entry->hash == skip_shader_ || (ps && ps->entry->hash == skip_shader_))) {
    return true;
  }
  if (arm_) {
    const uint64_t ps_hash = ps ? ps->entry->hash : 0;
    if (arm_target_) {
      if ((vs->entry->hash == arm_target_ || ps_hash == arm_target_) && arm_new_.empty()) {
        arm_new_.emplace_back(vs->entry->hash, ps_hash);
      }
    } else if (arm_pairs_.insert(vs->entry->hash * 0x9E3779B97F4A7C15ull ^ ps_hash).second && !arm_learn_) {
      arm_new_.emplace_back(vs->entry->hash, ps_hash);
    }
  }
  if (trace_) {
    std::string line = fmt::format("draw #{} vs={:016X} ps={:016X} prim={} n={} mode={} pitch={} h={} cc={:X} cm={:X} bm={:X}",
                                   draw_index,
                                   vs->entry->hash, ps ? ps->entry->hash : 0, uint32_t(prim_type),
                                   index_count, uint32_t(edram_mode), pitch, height,
                                   regs[XE_GPU_REG_RB_COLORCONTROL], color_mask, bin_mask_);
    if (packet_predicated_) line += " pred";
    for (uint32_t i = 0; i < color_count; ++i) {
      reg::RB_COLOR_INFO info;
      info.value = regs[kColorInfo[i]];
      line += fmt::format(" rt{}={:X}/f{}", i, uint32_t(info.color_base), uint32_t(info.color_format));
    }
    if (depth) {
      line += fmt::format(" ds={:X}/f{} z={}{}{} f{}", uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base),
                          uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format),
                          depth_control.z_enable ? "t" : "", depth_control.z_write_enable ? "w" : "",
                          depth_control.stencil_enable ? "s" : "", uint32_t(depth_control.zfunc));
    }
    for (uint32_t slot = 0; slot < 32; ++slot) {
      xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
      if (fetch.type != xenos::FetchConstantType::kTexture || !fetch.base_address) continue;
      uint32_t t_base, t_mip, t_top, t_last;
      gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &t_base,
                                                          &t_mip, &t_top, &t_last);
      line += fmt::format(
          " t{}={:08X}/f{}/{}x{}/d{}/L{}-{}/c{}{}{}/f{}{}{}/a{}/b{}{}{}/sw{:03X}", slot,
          fetch.base_address << 12,
          uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
          uint32_t(fetch.dimension), t_top, t_last, uint32_t(fetch.clamp_x),
          uint32_t(fetch.clamp_y), uint32_t(fetch.clamp_z), uint32_t(fetch.mag_filter),
          uint32_t(fetch.min_filter), uint32_t(fetch.mip_filter), uint32_t(fetch.aniso_filter),
          int32_t(fetch.lod_bias),
          fetch.stacked ? fmt::format("/s{}", uint32_t(fetch.size_2d.stack_depth) + 1) : "",
          FindResolved(fetch.base_address << 12) ? "/R" : "", uint32_t(fetch.swizzle) & 0xFFF);
    }
    Trace("{}", line);
    Trace("  depthcontrol={:08X} stencilrefmask={:08X} bf={:08X} sqconst vs={:08X} ps={:08X} bool={:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}",
          regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_STENCILREFMASK],
          regs[XE_GPU_REG_RB_STENCILREFMASK_BF], regs[XE_GPU_REG_SQ_VS_CONST], regs[XE_GPU_REG_SQ_PS_CONST],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 1],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 2], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 3],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 4], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 5],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 6], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 7]);
    Trace("  raster mode={:08X} clip={:08X} offset=({},{},{},{}) blend={:08X} constants=({},{},{},{}) alpha={}",
          regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL], regs[XE_GPU_REG_PA_CL_CLIP_CNTL],
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET), regs[XE_GPU_REG_RB_BLENDCONTROL0],
          regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
          regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
          regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF));
    // What a stacked fetch is actually handed: the pixel shader constants,
    // among them the one whose Z picks the slice.
    if (ps) {
      bool stacked = false;
      for (uint32_t slot = 0; slot < 32; ++slot) {
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
        stacked = stacked || (fetch.type == xenos::FetchConstantType::kTexture && fetch.stacked &&
                              fetch.base_address);
      }
      if (stacked) {
        std::string constants;
        for (uint32_t i = 0; i < 4; ++i) {
          constants += fmt::format(" c{}=({},{},{},{})", i,
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 0),
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 1),
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 2),
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 3));
        }
        Trace("  ps{:016X}{}", ps->entry->hash, constants);
      }
      // A material that samples the shadow cascades (depth resolves in slots
      // 13-15) picks one by view depth, so the split shows up on the ground as
      // a straight screen line. Its constants, once per shader and frame, give
      // where the splits fall and how each cascade is addressed.
      bool cascades = false;
      for (uint32_t slot = 13; slot < 16; ++slot) {
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
        const auto format = xenos::TextureFormat(uint32_t(fetch.format));
        cascades = cascades || (fetch.type == xenos::FetchConstantType::kTexture && fetch.base_address &&
                                (format == xenos::TextureFormat::k_24_8 ||
                                 format == xenos::TextureFormat::k_24_8_FLOAT ||
                                 format == xenos::TextureFormat::k_16_FLOAT));
      }
      // <files>/plume-trace-ps names one more pixel shader (hex) whose
      // constants go into the trace at every draw, band by band.
      const bool chosen = trace_ps_ && ps->entry->hash == trace_ps_;
      if (chosen || (cascades && traced_cascade_ps_.insert(ps->entry->hash).second)) {
        std::string constants;
        for (uint32_t i = 0; i < 256; ++i) {
          float c[4];
          for (uint32_t j = 0; j < 4; ++j) c[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + j);
          if (!c[0] && !c[1] && !c[2] && !c[3]) continue;
          constants += fmt::format(" c{}=({},{},{},{})", i, c[0], c[1], c[2], c[3]);
        }
        Trace("  psconst ps{:016X}{}", ps->entry->hash, constants);
      }
    }
    const auto* declared = g_shaderInputElements + vs->entry->inputElementOffset;
    for (uint32_t i = 0; i < vs->entry->inputElementCount; ++i) {
      const auto& e = declared[i];
      if (size_t(e.fetchAddress) * 3 + 2 >= vs->ucode_dword_count()) continue;
      const auto* f = vs->ucode_dwords() + size_t(e.fetchAddress) * 3;
      Trace("  fetch L{} @{} {:08X} {:08X} {:08X} exp={}", e.location, e.fetchAddress,
            f[0], f[1], f[2], int32_t(f[1] << 2) >> 26);
    }
  }

  // Constants: the guest register file, exactly as the recompiled shaders
  // address it (vertex c0..c255, pixel c256..c511 as their c0..c255).
  // The block is assembled here and only goes into the ring once it is whole,
  // so that a draw whose constants did not change can keep the last one.
  // Not cleared between draws: every field a shader reads is either written
  // for each draw or only read by the draws that write it (the live fetches,
  // exports, point sizes and clip planes), and what an earlier draw left in the
  // others keeps the second half the same from draw to draw.
  uint8_t* sh = shared_built_;
  // plume-skip bit 16777216 points every fetch at the 1x1 white texture in
  // slot 0: the sampling instructions stay, the texture bandwidth goes.
  if (skip_mask_ & 16777216) {
    std::memset(texture_descriptor, 0, sizeof(texture_descriptor));
    std::memset(cube_descriptor, 0, sizeof(cube_descriptor));
    std::memset(stack_descriptor, 0, sizeof(stack_descriptor));
  }
  // plume-debug-shader mode 16 points every 2D fetch but slot 0 at white, 32
  // only slot 0: which texture of a draw takes its output away.
  if (debug_shader_ && (vs->entry->hash == debug_shader_ || (ps && ps->entry->hash == debug_shader_))) {
    if (debug_mode_ & 16) {
      for (uint32_t i = 1; i < 32; ++i) texture_descriptor[i] = 0;
    }
    if (debug_mode_ & 32) texture_descriptor[0] = 0;
  }
  for (uint32_t mask = white_slots_; mask; mask &= mask - 1) texture_descriptor[__builtin_ctz(mask)] = 0;
  std::memcpy(sh + RENUT_SHARED_TEXTURE2D_OFFSET, texture_descriptor, sizeof(texture_descriptor));
  std::memcpy(sh + RENUT_SHARED_TEXTURECUBE_OFFSET, cube_descriptor, sizeof(cube_descriptor));
  std::memcpy(sh + RENUT_SHARED_TEXTURE3D_OFFSET, stack_descriptor, sizeof(stack_descriptor));
  std::memcpy(sh + RENUT_SHARED_SAMPLER_OFFSET, sampler_descriptor, sizeof(sampler_descriptor));
  std::memcpy(sh + RENUT_SHARED_BOOLEANS_OFFSET, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              RENUT_BOOL_REGISTER_COUNT / 8);
  if (loop_constants_dirty_) {
    loop_constants_dirty_ = false;
    for (uint32_t i = 0; i < RENUT_LOOP_CONSTANT_COUNT; ++i) {
      uint32_t v = regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
      int32_t loop[4] = {int32_t(v & 0xFF), int32_t((v >> 8) & 0xFF), int32_t(int8_t((v >> 16) & 0xFF)), 0};
      std::memcpy(sh + RENUT_SHARED_LOOP_OFFSET + i * 16, loop, 16);
    }
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const bool alpha_test_enabled = color_control.alpha_test_enable &&
                                  color_control.alpha_func != xenos::CompareFunction::kAlways;
  // Alpha to coverage (N&B's grass and foliage) has no samples to cover
  // without MSAA; like a single-sample A2C, keep the fragments at or above
  // half coverage.
  const bool alpha_to_mask = color_control.alpha_to_mask_enable && !(skip_mask_ & 4096);
  bool alpha_test = ps && (alpha_test_enabled || alpha_to_mask);
  float alpha_ref = alpha_test_enabled ? regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF) : 0.5f;
  // The pixel shaders only clip(alpha - ref), i.e. pass at alpha >= ref. N&B's
  // alpha tests are GREATER (97% of the traced ones, mostly against 0) and
  // GREATER_EQUAL, so GREATER takes the next float up: against 0 the shader
  // used to keep the fully transparent texels (the world portal's photo).
  // Not the denormal after 0, which the GPU may flush to zero.
  if (alpha_test_enabled && color_control.alpha_func == xenos::CompareFunction::kGreater) {
    alpha_ref = std::max(std::nextafter(alpha_ref, INFINITY), FLT_MIN);
  }
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 12, &alpha_ref, 4);

  // Vertex inputs: the layout the bound vertex shader's vfetches describe
  // (DecodeLiveElements); the streams they name are the guest's vertex fetch
  // constants 95 - stream.
  // Kept between draws so their storage is not allocated again every draw.
  std::vector<RenderInputElement>& input_elements = draw_input_elements_;
  std::vector<RenderInputSlot>& input_slots = draw_input_slots_;
  input_elements.clear();
  input_slots.clear();
  std::array<uint64_t, 17> slot_offsets{};
  std::array<RenderBuffer*, 17> slot_buffers{};
  std::array<uint32_t, 17> slot_sizes{};
  std::array<bool, 17> slot_used{};
  uint32_t swapped[7] = {};  // texcoord, normal, binormal, tangent, blendweight, position, blendindices
  uint32_t input_remap[RENUT_INPUT_REMAP_COUNT] = {};
  uint32_t packed_inputs = 0, int_inputs = 0;
  bool r11g11b10_normals = false;
  uint64_t streams[RENUT_VERTEX_STREAM_COUNT] = {};
  for (uint32_t stream = 0; stream < RENUT_VERTEX_STREAM_COUNT; ++stream) {
    // The last entry is fetch constant 0, which Direct3D's own Clear uses.
    xenos::xe_gpu_vertex_fetch_t fetch =
        regs.GetVertexFetch(stream == RENUT_DIRECT_FETCH_STREAM ? 0 : 95 - stream);
    if (fetch.type != xenos::FetchConstantType::kVertex) continue;
    uint64_t offset;
    RenderBuffer* buffer;
    Trace("  stream{} {:08X}+{:X} e{}", stream, fetch.address << 2, fetch.size << 2, uint32_t(fetch.endian));
    if (trace_ && fetch.size && (fetch.size << 2) <= (1u << 20)) {
      const auto* guest = memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2);
      traced_streams_.push_back({frame_draw_ - 1, stream, uint32_t(fetch.address << 2),
                                 std::vector<uint8_t>(guest, guest + (fetch.size << 2))});
    }
    // A stream with format 17 vectors the input assembler hands to the shader
    // (see RepackFormat17) is copied for this draw with those moved into the
    // layout the shader decodes.
    if (!vs->repack_decoded && vs->compiled) {
      vs->repack_decoded = true;
      for (const LiveElement& e : vs->elements) {
        if (e.format != 17 || e.slot >= 17 || !e.stride || vs->repack[e.slot].size() == 8) continue;
        for (const auto& [location, integer] : vs->compiled->inputs) {
          if (location == e.location && !integer) {
            vs->repack[e.slot].emplace_back(e.offset, e.stride);
            vs->repack_signature[e.slot] =
                (vs->repack_signature[e.slot] ^ ((uint64_t(e.offset) << 32) | e.stride)) * 0x100000001B3ull + 1;
            break;
          }
        }
      }
    }
    const bool repackable = !patch_list && stream != RENUT_DIRECT_FETCH_STREAM && vs->compiled &&
                            !vs->repack[stream].empty();
    // What the last draw found for this stream is good again when its fetch
    // constant, the native stream window and the repack are the same and no
    // kept buffer went away, the ring has not wrapped, and no export range
    // overlaps (exports change under draws).
    const BufferRange native_stream =
        native_swapped_ && stream < native_streams_.size() ? native_streams_[stream] : BufferRange{};
    StreamMemo& memo = stream_memo_[stream];
    const uint32_t* constant_words = reinterpret_cast<const uint32_t*>(&fetch);
    const uint64_t signature = repackable ? vs->repack_signature[stream] : 0;
    bool bound = false;
    if (memo.buffer && memo.epoch == stream_epoch_ && memo.generation == upload_generation_ &&
        memo.words[0] == constant_words[0] && memo.words[1] == constant_words[1] &&
        memo.native_base == native_stream.base &&
        memo.native_size == native_stream.size && memo.repack_signature == signature &&
        memo.export_epoch == export_epoch_ && !validate_windows_ &&
        !sync_draws_ && !trace_) {
      buffer = memo.buffer;
      offset = memo.offset;
      bound = memo.bound;
      ++stats_.stream_memo_hits;
    } else if (PooledStream* pooled = objkeys_ && !validate_windows_ && !sync_draws_ && !trace_
                                          ? &stream_pool_[((uint64_t(constant_words[0]) * 0x9E3779B97F4A7C15ull) ^
                                                           (uint64_t(constant_words[1]) * 0xC2B2AE3D27D4EB4Full) ^
                                                           native_stream.base ^ signature) %
                                                          kStreamPoolSize]
                                          : nullptr;
               pooled && pooled->frame == swaps_ && pooled->memo.buffer && pooled->memo.epoch == stream_epoch_ &&
               pooled->memo.generation == upload_generation_ && pooled->memo.words[0] == constant_words[0] &&
               pooled->memo.words[1] == constant_words[1] && pooled->memo.native_base == native_stream.base &&
               pooled->memo.native_size == native_stream.size && pooled->memo.repack_signature == signature &&
               pooled->memo.export_epoch == export_epoch_) {
      // Another slot, or an earlier pass, resolved this same stream this frame.
      memo = pooled->memo;
      buffer = memo.buffer;
      offset = memo.offset;
      bound = memo.bound;
      ++stats_.stream_memo_hits;
    } else {
      bool repacked = false;
      if (repackable && !ExportOverlaps(fetch.address << 2, fetch.size << 2)) {
        buffer = upload_.get();
        repacked = RepackedStream(fetch.address << 2, fetch.size << 2, fetch.endian, vs->repack[stream].data(),
                                  uint32_t(vs->repack[stream].size()), buffer, offset);
        if (repacked) Trace("  stream{} repacked: {} format 17 element(s)", stream, vs->repack[stream].size());
      }
      stream_object_ = native_stream;
      bound = repacked || BindStream(fetch.address << 2, fetch.size << 2, fetch.endian, buffer, offset);
      stream_object_ = {};
      if (!exports_.empty() && ExportOverlaps(fetch.address << 2, fetch.size << 2)) {
        memo.buffer = nullptr;  // an exported stream is not remembered
      } else {
        std::memcpy(memo.words, constant_words, sizeof(memo.words));
        memo.native_base = native_stream.base;
        memo.native_size = native_stream.size;
        memo.repack_signature = signature;
        memo.epoch = stream_epoch_;
        memo.export_epoch = export_epoch_;
        memo.generation = upload_generation_;
        memo.buffer = bound ? buffer : nullptr;
        memo.offset = offset;
        memo.bound = bound;
        if (objkeys_ && memo.buffer && !validate_windows_ && !sync_draws_ && !trace_) {
          PooledStream& pooled = stream_pool_[((uint64_t(constant_words[0]) * 0x9E3779B97F4A7C15ull) ^
                                               (uint64_t(constant_words[1]) * 0xC2B2AE3D27D4EB4Full) ^
                                               native_stream.base ^ signature) %
                                              kStreamPoolSize];
          pooled.memo = memo;
          pooled.frame = swaps_;
        }
      }
    }
    if (bound) {
      streams[stream] = buffer == upload_.get() ? Address(offset) : buffer->getDeviceAddress() + offset;
      slot_offsets[stream] = offset;
      slot_buffers[stream] = buffer;
      slot_sizes[stream] = fetch.size << 2;
    }
  }
  std::memcpy(sh + RENUT_SHARED_STREAMS_OFFSET, streams, sizeof(streams));
  if (validate_windows_) {
    for (uint32_t s = 0; s < RENUT_VERTEX_STREAM_COUNT; ++s) {
      if (!slot_sizes[s] || !slot_buffers[s]) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(s == RENUT_DIRECT_FETCH_STREAM ? 0 : 95 - s);
      ValidateRange(fmt::format("stream{}", s).c_str(), vs->entry->hash, slot_buffers[s], slot_offsets[s],
                    fetch.address << 2, slot_sizes[s], fetch.endian);
    }
  }
  if (sync_draws_) {
    // What this draw hands the GPU, kept for the line a failure prints.
    std::string info = fmt::format("vs={:016X} ps={:016X} index_count={} streams:", vs->entry->hash,
                                   ps ? ps->entry->hash : 0, index_count);
    for (uint32_t s = 0; s < slot_sizes.size(); ++s) {
      if (!slot_sizes[s]) continue;
      info += fmt::format(" {}:{}B/{}@{:X}", s, slot_sizes[s], slot_buffers[s] == upload_.get() ? "ring" : "kept",
                          s < RENUT_VERTEX_STREAM_COUNT ? streams[s] : 0);
    }
    for (uint32_t s = 0; s < RENUT_VERTEX_STREAM_COUNT; ++s) {
      if (s == RENUT_DIRECT_FETCH_STREAM || !slot_sizes[s] || slot_sizes[s] > 256) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - s);
      std::vector<uint8_t> bytes(slot_sizes[s]);
      CopySwapped(bytes.data(), memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2), slot_sizes[s],
                  fetch.endian);
      info += fmt::format(" data{}:", s);
      for (uint32_t i = 0; i + 4 <= bytes.size() && i < 64; i += 4) {
        float f;
        std::memcpy(&f, bytes.data() + i, 4);
        info += fmt::format(" {:.3g}", f);
      }
    }
    // Whether each kept window still holds what the guest holds: the GPU reads
    // the window, the checks below read the guest. A window the watch missed
    // would give the shader another mesh's bytes.
    for (uint32_t s = 0; s < RENUT_VERTEX_STREAM_COUNT; ++s) {
      if (s == RENUT_DIRECT_FETCH_STREAM || !slot_sizes[s] || !slot_buffers[s] || slot_buffers[s] == upload_.get()) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - s);
      std::vector<uint8_t> guest(slot_sizes[s]);
      CopySwapped(guest.data(), memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2), slot_sizes[s], fetch.endian);
      const auto* window = static_cast<const uint8_t*>(slot_buffers[s]->map());
      if (!window) continue;
      uint32_t differ = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < guest.size(); ++i) {
        if (window[slot_offsets[s] + i] != guest[i]) {
          if (first == UINT32_MAX) first = i;
          ++differ;
        }
      }
      slot_buffers[s]->unmap();
      if (differ) {
        info += fmt::format(" STALE{}[{}B differ of {}, first at {}]", s, differ, guest.size(), first);
        ++sync_stale_streams_;
      }
    }
    // What the integer attributes hold over the vertices this draw names: a
    // blend index past the palette is a read past the stream, which the Mali
    // faults on (the stream is read through a device address).
    if (!patch_list && vs->compiled) {
      std::vector<uint32_t> used;
      const bool indexed = index_buffer_info && index_buffer_info->guest_base;
      if (indexed) {
        const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
        std::vector<uint8_t> raw(size_t(index_count) * (is32 ? 4 : 2));
        CopySwapped(raw.data(), memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base),
                    uint32_t(raw.size()), index_buffer_info->endianness);
        for (uint32_t i = 0; i < index_count; ++i) {
          used.push_back(is32 ? reinterpret_cast<const uint32_t*>(raw.data())[i]
                              : reinterpret_cast<const uint16_t*>(raw.data())[i]);
        }
      } else {
        for (uint32_t i = 0; i < index_count && i < 65536; ++i) used.push_back(i);
      }
      uint32_t max_vertex = 0;
      for (uint32_t v : used) max_vertex = std::max(max_vertex, v);
      const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
      info += fmt::format(" max_index={} base={} palette={}", max_vertex, base_vertex, slot_sizes[10] / 64);
      for (const LiveElement& e : vs->elements) {
        if (e.normalized || !e.stride || (e.format != 6 && e.format != 25 && e.format != 26)) continue;
        xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - e.slot);
        if (fetch.size == 0 || (fetch.size << 2) > (4u << 20)) continue;
        std::vector<uint8_t> stream(fetch.size << 2);
        CopySwapped(stream.data(), memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2),
                    uint32_t(stream.size()), fetch.endian);
        const uint32_t width = e.format == 6 ? 1 : 2;
        const uint32_t count = e.format == 26 ? 4 : (e.format == 25 ? 2 : 4);
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        uint32_t outside = 0;
        for (uint32_t v : used) {
          const int64_t at = (int64_t(v) + base_vertex) * e.stride + e.offset;
          if (at < 0 || uint64_t(at) + count * width > stream.size()) {
            ++outside;
            continue;
          }
          for (uint32_t c = 0; c < count; ++c) {
            const uint8_t* p = stream.data() + at + c * width;
            int32_t value;
            if (width == 1) {
              value = e.is_signed ? int32_t(int8_t(p[0])) : int32_t(p[0]);
            } else {
              uint16_t w;
              std::memcpy(&w, p, 2);
              value = e.is_signed ? int32_t(int16_t(w)) : int32_t(w);
            }
            lo = std::min(lo, value);
            hi = std::max(hi, value);
          }
        }
        info += fmt::format(" L{}[f{} {}..{} outside={}]", e.location, e.format, lo, hi, outside);
      }
    }
    sync_draw_info_ = std::move(info);
  }

  // Memory export. What a shader exports into is named by the stream
  // constants it was handed - Direct3D's own layout, recognisable by its bit
  // pattern (see renutMemExport) - and each of those streams becomes an
  // exported range. The shader stores only into the ranges listed here.
  if (vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT) {
    uint32_t ranges[RENUT_EXPORT_RANGE_COUNT * 4] = {};
    uint32_t range_count = 0;
    const uint32_t* constants = &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X];
    // Only the constants the shader builds its addresses from: the register
    // file still holds the stream constants of earlier draws, and a range made
    // from one of those would shadow guest memory the CPU is still writing.
    const uint32_t flags = vs->entry->specConstantsMask;
    const uint32_t last = std::min<uint32_t>(RENUT_SHADER_EXPORT_LAST(flags), 255);
    for (uint32_t i = RENUT_SHADER_EXPORT_FIRST(flags); i <= last && range_count < RENUT_EXPORT_RANGE_COUNT;
         ++i) {
      const uint32_t* k = constants + i * 4;
      if ((k[0] >> 30) != 1 || (k[1] >> 23) != 0x96 || (k[2] >> 23) != 0x96 || (k[3] >> 23) != 0x96) {
        continue;
      }
      const uint32_t element_bytes = ExportElementBytes((k[2] >> 8) & 0x3F);
      const uint32_t elements = k[3] & 0x7FFFFF;
      const uint32_t base = k[0] << 2;
      if (!element_bytes || !elements || base >= 0x20000000u ||
          uint64_t(elements) * element_bytes > (32u << 20)) {
        continue;
      }
      Trace("  eA c{} {:08X} {:08X} {:08X} {:08X} -> {:08X}+{:X} f{}", i, k[0], k[1], k[2], k[3], base,
            elements * element_bytes, (k[2] >> 8) & 0x3F);
      const uint32_t endian_bits = k[2] & 7;
      ExportRange* range = GetExport(base, elements * element_bytes,
                                     endian_bits <= 3 ? xenos::Endian(endian_bits) : xenos::Endian::k8in32);
      if (!range) continue;
      if (range->read) ExportBarrier();
      range->written = true;
      range->exported_frame = swaps_;
      bool listed = false;
      for (uint32_t r = 0; r < range_count; ++r) listed |= ranges[r * 4] == range->base;
      if (listed) continue;
      ranges[range_count * 4 + 0] = range->base;
      ranges[range_count * 4 + 1] = range->size;
      ranges[range_count * 4 + 2] = uint32_t(range->address);
      ranges[range_count * 4 + 3] = uint32_t(range->address >> 32);
      ++range_count;
    }
    std::memcpy(sh + RENUT_SHARED_EXPORT_OFFSET, ranges, sizeof(ranges));
    ++stats_.export_draws;
    ++export_draws_total_;
    if (trace_) {
      std::string line = "  export";
      for (uint32_t r = 0; r < range_count; ++r) {
        line += fmt::format(" {:08X}+{:X}", ranges[r * 4], ranges[r * 4 + 1]);
      }
      Trace("{}", line);
    }
  }
  for (const auto& [k, entry] : patch_list ? vs->domain_live_fetches : vs->live_fetches) {
    std::memcpy(sh + RENUT_SHARED_FETCH_OFFSET + k * 16, entry.data(), 16);
  }
  bool zero_used = false;
  // A patch draw has one input: where in its patch the generated vertex sits.
  // What the guest's shader fetches, it fetches by patch index through the
  // table above.
  if (patch_list) {
    input_elements.emplace_back("TEXCOORD", RENUT_DOMAIN_INPUT_LOCATION,
                                RENUT_DOMAIN_INPUT_LOCATION, RenderFormat::R32G32B32A32_FLOAT,
                                kPatchDomainBinding, 0);
    input_slots.emplace_back(kPatchDomainBinding, uint32_t(4 * sizeof(float)));
  }
  for (auto [location, integer] : vs->compiled->inputs) {
    if (patch_list) break;
    const LiveElement* element = nullptr;
    for (const LiveElement& e : vs->elements) {
      if (e.location == location) {
        element = &e;
        break;
      }
    }
    InputDecode decode = InputDecode::kNone;
    RenderFormat format = element && !integer ? VertexFormat(element->format, element->is_signed,
                                                             element->normalized, decode)
                                              : RenderFormat::UNKNOWN;
    if (!element || format == RenderFormat::UNKNOWN || !element->stride ||
        !slot_sizes[element->slot] ||
        (slot_used[element->slot] &&
         std::find_if(input_slots.begin(), input_slots.end(), [&](const RenderInputSlot& s) {
           return s.index == element->slot && s.stride != element->stride;
         }) != input_slots.end())) {
      input_elements.emplace_back("TEXCOORD", location, location,
                                  integer ? RenderFormat::R32G32B32A32_UINT
                                          : RenderFormat::R32G32B32A32_FLOAT,
                                  kZeroBinding, 0);
      zero_used = true;
      continue;
    }
    input_elements.emplace_back("TEXCOORD", location, location, format, element->slot,
                                element->offset);
    switch (decode) {
      case InputDecode::kInteger: int_inputs |= 1u << location; break;
      case InputDecode::kPacked1010102: packed_inputs |= 1u << location; break;
      case InputDecode::kPacked111110:
        // tfetchR11G11B10 decodes these, for normals, tangents and binormals only.
        r11g11b10_normals |= element->usage == 3 || element->usage == 6 || element->usage == 7;
        break;
      default: break;
    }
    if (!slot_used[element->slot]) {
      slot_used[element->slot] = true;
      input_slots.emplace_back(element->slot, element->stride);
    }
    // After the k8in32 swap the input assembler reads the hardware's component
    // order (X in the low half), and Direct3D's patched vfetch swizzle (.yxwz)
    // puts 16-bit halves back in memory order. A shader recompiled with that
    // swizzle needs nothing more - swapping every 16-bit element here mangled
    // every mesh with half-float positions. One recompiled without it gets the
    // difference remapped (see FetchRemap).
    if (location < RENUT_INPUT_REMAP_COUNT) input_remap[location] = element->remap;
    // Debug: plume-skip bit 512 reverses every 16-bit pair a second time, the
    // old behaviour.
    if (IsSixteenBitFormat(element->format) && (skip_mask_ & 512)) {
      // Debug: the old behaviour, reversing the 16-bit pairs a second time.
      int which = -1;
      switch (element->usage) {
        case 5: which = 0; break;   // TEXCOORD
        case 3: which = 1; break;   // NORMAL
        case 7: which = 2; break;   // BINORMAL
        case 6: which = 3; break;   // TANGENT
        case 1: which = 4; break;   // BLENDWEIGHT
        case 0: which = 5; break;   // POSITION
        case 2: which = 6; break;   // BLENDINDICES
        default: break;
      }
      if (which >= 0) swapped[which] |= 1u << element->usage_index;
    }
  }
  if (sync_draws_ && !patch_list && vs->compiled && sync_diag_[vs->entry->hash]++ < 1) {
    // One line per vertex shader: what its fetches can reach, for finding the
    // draw that reads past a stream (the Mali faults where other GPUs read on).
    std::string diag = fmt::format("Renut: Plume draw diag vs={:016X} ps={:016X} count={} indexed={} streams:",
                                   vs->entry->hash, ps ? ps->entry->hash : 0, index_count,
                                   index_buffer_info && index_buffer_info->guest_base);
    for (uint32_t s = 0; s < slot_sizes.size(); ++s) {
      if (slot_sizes[s]) diag += fmt::format(" {}:{}B@{:X}", s, slot_sizes[s], s < RENUT_VERTEX_STREAM_COUNT ? streams[s] : 0);
    }
    diag += " live:";
    for (const auto& [k, entry] : vs->live_fetches) {
      diag += fmt::format(" #{}(stream {} fmt {} stride {} off {})", k, entry[0] & 0xFF, (entry[0] >> 8) & 0xFF,
                          entry[1], entry[2]);
    }
    // The vertices the draw names, then the extremes of each integer attribute.
    std::vector<uint32_t> used;
    if (index_buffer_info && index_buffer_info->guest_base) {
      const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
      std::vector<uint8_t> raw(size_t(index_count) * (is32 ? 4 : 2));
      CopySwapped(raw.data(), memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base),
                  uint32_t(raw.size()), index_buffer_info->endianness);
      for (uint32_t i = 0; i < index_count; ++i) {
        used.push_back(is32 ? reinterpret_cast<const uint32_t*>(raw.data())[i]
                            : reinterpret_cast<const uint16_t*>(raw.data())[i]);
      }
    } else {
      for (uint32_t i = 0; i < index_count && i < 65536; ++i) used.push_back(i);
    }
    uint32_t max_vertex = 0;
    for (uint32_t v : used) max_vertex = std::max(max_vertex, v);
    diag += fmt::format(" max_index={} base_vertex={} elements:", max_vertex, int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]));
    for (const LiveElement& e : vs->elements) {
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - e.slot);
      diag += fmt::format(" L{}(slot {} fmt {}{} stride {} off {} cap {})", e.location, e.slot, e.format,
                          e.is_signed ? "s" : "", e.stride, e.offset, e.stride ? (fetch.size << 2) / e.stride : 0);
      if (e.format != 26 || e.normalized || !e.stride || fetch.size == 0) continue;
      std::vector<uint8_t> stream(fetch.size << 2);
      CopySwapped(stream.data(), memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2),
                  uint32_t(stream.size()), fetch.endian);
      int32_t lo = INT32_MAX, hi = INT32_MIN;
      for (uint32_t v : used) {
        const uint64_t at = uint64_t(int64_t(v) + int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET])) * e.stride + e.offset;
        if (at + 8 > stream.size()) continue;
        const int16_t* comps = reinterpret_cast<const int16_t*>(stream.data() + at);
        for (int c = 0; c < 4; ++c) {
          lo = std::min<int32_t>(lo, comps[c]);
          hi = std::max<int32_t>(hi, comps[c]);
        }
      }
      diag += fmt::format("[int16 range {}..{}]", lo, hi);
    }
    REXLOG_INFO("{}", diag);
  }
  if (trace_) {
    std::string line = "  inputs";
    for (auto [location, integer] : vs->compiled->inputs) {
      const LiveElement* e = nullptr;
      for (const LiveElement& l : vs->elements) {
        if (l.location == location) e = &l;
      }
      if (!e) {
        line += fmt::format(" L{}{}=none", location, integer ? "u" : "f");
        continue;
      }
      const auto fetch = regs.GetVertexFetch(95 - e->slot);
      line += fmt::format(" L{}{}=u{}.{}/f{}{}{}/s{}+{}/e{}", location, integer ? "u" : "f", e->usage,
                          e->usage_index, e->format, e->is_signed ? "s" : "", e->normalized ? "n" : "",
                          e->stride, e->offset, uint32_t(fetch.endian));
    }
    Trace("{}", line);
    // The first vertices' dwords as the host reads them (already swapped), by
    // stream, and the fetches the shader does on its own.
    for (uint32_t stream = 0; stream < slot_sizes.size(); ++stream) {
      if (!slot_sizes[stream] || slot_buffers[stream] != upload_.get()) continue;
      const auto* words = reinterpret_cast<const uint32_t*>(Pointer(slot_offsets[stream]));
      line = fmt::format("  data{}", stream);
      // All of it for the shader an armed trace is after.
      const uint32_t shown = vs->entry->hash == arm_target_ ? 4096 : 16;
      for (uint32_t i = 0; i < std::min<uint32_t>(shown, slot_sizes[stream] / 4); ++i) {
        line += fmt::format(" {:08X}", words[i]);
      }
      Trace("{}", line);
    }
    line = "  vsc";
    for (uint32_t i = 0; i < 8; ++i) {
      line += fmt::format(" c{}=({},{},{},{})", i, regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 0),
                          regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 1),
                          regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 2),
                          regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 3));
    }
    Trace("{}", line);
    if (ps && (ps->entry->hash == debug_shader_ || ps->entry->hash == arm_target_)) {
      for (uint32_t i = 0; i < 32; ++i) {
        float c[4];
        for (uint32_t j = 0; j < 4; ++j) c[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + j);
        if (c[0] || c[1] || c[2] || c[3]) Trace("  psc{}=({},{},{},{})", i, c[0], c[1], c[2], c[3]);
      }
    }
    if (vs->entry->hash == debug_shader_ || vs->entry->hash == arm_target_) {
      // Every constant for the shader under study, to run it by hand.
      for (uint32_t i = 8; i < 256; ++i) {
        float c[4];
        for (uint32_t j = 0; j < 4; ++j) c[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + j);
        if (c[0] || c[1] || c[2] || c[3]) Trace("  vsc{}=({},{},{},{})", i, c[0], c[1], c[2], c[3]);
      }
      Trace("  bools {:08X} {:08X}", regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
            regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 1]);
    }
    for (const auto& [k, entry] : patch_list ? vs->domain_live_fetches : vs->live_fetches) {
      float scale;
      std::memcpy(&scale, &entry[3], 4);
      Trace("  live{} stream={} format={} flags={} stride={} remap={:X} offset={} scale={}", k,
            entry[0] & 0xFF, (entry[0] >> 8) & 0xFF, entry[0] >> 16, entry[1] & 0xFFFF, entry[1] >> 16,
            entry[2], scale);
    }
  }
  if (skip_mask_) {
    bool skinned = false, half_position = false;
    for (const LiveElement& e : vs->elements) {
      skinned |= e.usage == 2;
      half_position |= e.usage == 0 && e.format == 32;
    }
    const int32_t base = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
    if (((skip_mask_ & 1) && int_inputs) || ((skip_mask_ & 2) && packed_inputs) ||
        ((skip_mask_ & 4) && skinned) || ((skip_mask_ & 8) && half_position) ||
        ((skip_mask_ & 16) && quad_list) || ((skip_mask_ & 32) && base != 0) ||
        ((skip_mask_ & 64) && !skinned) || ((skip_mask_ & 128) && zero_used)) {
      return true;
    }
  }
  if (skin_log_) {
    bool skinned = false;
    for (const LiveElement& e : vs->elements) skinned |= e.usage == 2;
    if (skinned) {
      std::lock_guard<std::mutex> lock(skinned_mutex_);
      SkinnedSeen& seen = skinned_seen_[vs->entry->hash];
      if (!seen.draws++) seen.first_frame = swaps_;
      seen.last_frame = swaps_;
      if (skin_skip_.count(vs->entry->hash)) return true;
    }
  }
  if (zero_used) input_slots.emplace_back(kZeroBinding, 0);
  // The input remaps, the swapped, packed and integer input masks go to the
  // shaders as specialization constants, below with the pipeline key.

  // Viewport. With the guest's viewport transform enabled the shader emits
  // clip space and the host viewport reproduces PA_CL_VPORT_*; with it off the
  // shader emits window coordinates and g_ViewportScaleOffset maps them.
  // XenosRecomp compiles with -fvk-invert-y, so the shaders already negate Y:
  // window y = yo + ys * ndc becomes a viewport of height -2 * ys at yo + ys.
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const Target* extent = colors[0] ? colors[0] : depth;
  // Its size in guest pixels: a shadow map is drawn at a fraction of it.
  const uint32_t guest_width = extent ? (extent->width * 4 + extent->scale - 1) / extent->scale : 0;
  const uint32_t guest_height = extent ? (extent->height * 4 + extent->scale - 1) / extent->scale : 0;
  // Window offset (predicated tiling moves each band to the target's origin)
  // and the D3D9 pixel centre at integer coordinates.
  float window_x = 0.f, window_y = 0.f;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    window_x = float(offset.window_x_offset);
    window_y = float(offset.window_y_offset);
  }
  const bool keep_pixel_center =
      pixel_center_debug_ == 0 ||
      (pixel_center_debug_ == 1 && edram_mode != xenos::EdramMode::kDepthOnly);
  if (keep_pixel_center && regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    window_x += 0.5f;
    window_y += 0.5f;
  }
  RenderViewport viewport;
  float scale_offset[4] = {1.f, 1.f, 0.f, 0.f};
  if (vte.vport_x_scale_ena) {
    float xs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
    float xo = vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.f;
    float ys = vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.f;
    float yo = vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.f;
    float zs = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.f;
    float zo = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.f;
    viewport = RenderViewport(xo - xs + window_x, yo + ys + window_y, 2.f * xs, -2.f * ys,
                              std::clamp(zo, 0.f, 1.f), std::clamp(zo + zs, 0.f, 1.f));
  } else {
    viewport = RenderViewport(window_x, window_y, float(guest_width), float(guest_height));
    scale_offset[0] = 2.f / float(guest_width);
    scale_offset[1] = -2.f / float(guest_height);
    scale_offset[2] = -1.f;
    scale_offset[3] = 1.f;
  }
  if (viewport.width <= 0.f) {
    viewport = RenderViewport(0.f, 0.f, float(guest_width), float(guest_height));
  }
  // Debug, split mode 5 (game_fixes.cpp): the band a predicated packet belongs
  // to, for the scissor below. Only predicated packets count - the mask stays
  // set between them. Off by default: with one tile the game predicates nearly
  // everything to mask 3, which then means the whole screen (the particles
  // lost their lower half to this scissor).
  const bool band_split_scene = g_renut_scene_split_mode.load(std::memory_order_relaxed) == 5 &&
                                !REXCVAR_GET(renut_scene_msaa) &&
                                regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 1280;
  const uint32_t draw_bands = band_split_scene && packet_predicated_ ? uint32_t(bin_mask_) & 0xF : 0;
  constexpr float kBandSplit = 384.f;  // 2x MSAA at 720p: bands of 384 and 336 rows
  std::memcpy(sh + RENUT_SHARED_VIEWPORT_OFFSET, scale_offset, sizeof(scale_offset));
  // Pixels that read their position relative to a band (renutFragPos): none
  // but with the debug switch below.
  float band_split = 0.f;
  // Debug (<files>/plume-band-fragpos "1"): with scene MSAA off, pixels below
  // row 384 of the scene read their position relative to the lower band, as
  // the console's lower-band replay gave them.
  if (band_fragpos_ && !REXCVAR_GET(renut_scene_msaa) && regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 1280 &&
      std::fabs(viewport.height) >= 700.f) {
    band_split = 384.f;
  }
  std::memcpy(sh + RENUT_SHARED_BAND_SPLIT_OFFSET, &band_split, sizeof(band_split));

  // User clip planes, in the guest's clip space; the vertex shaders turn the
  // enabled ones into a clip distance (renutUserClipDistance). N&B's
  // quarter-resolution mirrored pass clips at plane 0, and without it the
  // geometry on the far side of the plane filled the buffer the fog reads.
  // Which planes are on is a specialization constant (g_SpecUserClipMask).
  const auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const uint32_t ucp_mask = clip_cntl.clip_disable ? 0u : uint32_t(clip_cntl.ucp_ena);
  if (ucp_mask) {
    for (uint32_t i = 0; i < RENUT_USER_CLIP_PLANE_COUNT; ++i) {
      std::memcpy(sh + RENUT_SHARED_UCP_OFFSET + i * 16, &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 16);
    }
  }

  // Point sprites. The sizes are 12.4 fixed point radii in guest pixels; the
  // shaders want diameters. One unit of host NDC is half the viewport, so a
  // diameter over the viewport's extent is the radius in NDC - signed with the
  // viewport, so that -y is the top of the screen whichever way it is flipped.
  if (point_list) {
    const auto point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    const auto point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const float point[6] = {float(point_size.width) * (2.f / 16.f),
                            float(point_size.height) * (2.f / 16.f),
                            float(point_minmax.min_size) * (2.f / 16.f),
                            float(point_minmax.max_size) * (2.f / 16.f),
                            1.f / viewport.width, 1.f / viewport.height};
    std::memcpy(sh + RENUT_SHARED_POINT_OFFSET, point, sizeof(point));
    Trace("  points size={}x{} min={} max={} ndc/px={}x{} regs {:08X} {:08X}", point[0], point[1], point[2],
          point[3], point[4], point[5], point_size.value, point_minmax.value);
  }

  // The shared block is complete; place all three where the shaders will read
  // them. A block that overflows the ring flushes it, which drops the two
  // placed before it, so the generation says whether to lay them down again.
  // A vertex shader that reads constants by a0/aL (skinning) gets the whole
  // file placed as one block too, when rel_constants_mode_ asks for it. Patch
  // draws run the shader's domain variant, which has its own entry: they keep
  // the four blocks.
  // The refraction sprites (PS A8DCAC79: the swim splash, the wake) look up the
  // scene copy at the screen position plus a distortion, with the row clamped
  // to [c5.y, c5.z] - the part of the screen the tile being drawn covers. With
  // one tile (MSAA off) the game leaves (c5.y, c5.z) = (1, 0): the minimum
  // above the maximum pins every lookup to the top row, and the splash came out
  // as blocks of the sky's blue or the cliff's brown. The bounds are put in
  // order for this draw only; the register goes back afterwards.
  constexpr uint64_t kRefractionSpritePs = 0xA8DCAC7963881F1Cull;
  constexpr uint32_t kRefractionBoundsAt = XE_GPU_REG_SHADER_CONSTANT_256_X + 5 * 4;
  float refraction_saved[4] = {};
  bool refraction_fixed = false;
  if (refraction_fix_ && ps && ps->entry && ps->entry->hash == kRefractionSpritePs &&
      !REXCVAR_GET(renut_scene_msaa)) {
    float* bounds = reinterpret_cast<float*>(&register_file_->values[kRefractionBoundsAt]);
    if (bounds[1] > bounds[2]) {
      std::memcpy(refraction_saved, bounds, sizeof(refraction_saved));
      std::swap(bounds[1], bounds[2]);
      MarkConstantsWritten(kRefractionBoundsAt, 4);
      refraction_fixed = true;
    }
  }
  struct RefractionBoundsRestore {
    PlumeCommandProcessor* self;
    const float* saved;
    bool active;
    ~RefractionBoundsRestore() {
      if (!active) return;
      std::memcpy(&self->register_file_->values[kRefractionBoundsAt], saved, 4 * sizeof(float));
      self->MarkConstantsWritten(kRefractionBoundsAt, 4);
    }
  } refraction_restore{this, refraction_saved, refraction_fixed};
  uint32_t rel_variant = 0;
  if (rel_constants_mode_ && !patch_list && (vs->entry->specConstantsMask & RENUT_SHADER_FLAG_REL_CONSTANTS)) {
    rel_variant = rel_constants_mode_ < 10
                      ? rel_constants_mode_
                      : ((rel_toggle_[vs->entry->hash & 255]++ & 1) ? rel_constants_mode_ - 10 : 0);
  }
  const bool rel_active = rel_variant != 0;
  uint64_t generation;
  do {
    generation = upload_generation_;
    bool placed = true;
    // Depth-only draws have no fragment shader, so its constants can wait
    // until a color draw actually needs them.
    for (uint32_t stage = 0; stage < (ps || !shadow_native_constants_ ? 2u : 1u) && placed; ++stage) {
      uint8_t* shadow = stage ? ps_shadow_ : vs_shadow_;
      const auto* file = reinterpret_cast<const uint8_t*>(
          &regs.values[stage ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X]);
      for (uint32_t chunk = 0; chunk < kConstantChunks && placed; ++chunk) {
        placed = CommitConstants(shadow + chunk * kConstantChunkSize, file + chunk * kConstantChunkSize,
                                 kConstantChunkSize, constant_offsets_[stage][chunk],
                                 &constants_written_[stage][chunk]);
      }
    }
    if (placed && rel_active) {
      placed = CommitConstants(vs_all_shadow_,
                               reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]),
                               kStageConstantSize, vs_all_offset_, nullptr);
    }
    if (!placed ||
        !CommitConstants(shared_shadow_, shared_built_, RENUT_SHARED_HOT_SIZE, shared_offset_) ||
        !CommitConstants(shared_shadow_ + RENUT_SHARED_HOT_SIZE, shared_built_ + RENUT_SHARED_HOT_SIZE,
                         RENUT_SHARED_SIZE - RENUT_SHARED_HOT_SIZE, shared_cold_offset_)) {
      return Drop("constants upload", vs, ps);
    }
  } while (generation != upload_generation_);

  if (twin_dedupe_ && packet_predicated_ && !REXCVAR_GET(renut_scene_msaa)) {
    const uint64_t tiles = bin_mask_ & 0xF;
    // Only the draws seen recorded twice: screen-space rectangles (the
    // shadow multiply) and the water's patches. Objects the game splits by
    // band must all run, and the band jobs may draw look-alikes.
    const bool twin_kind = prim_type == xenos::PrimitiveType::kRectangleList ||
                           prim_type == xenos::PrimitiveType::kQuadList || patch_list;
    if (tiles != 0 && tiles != 0xF && twin_kind) {
      uint64_t parts[16];
      uint32_t n = 0;
      parts[n++] = vs->entry->hash;
      parts[n++] = ps ? ps->entry->hash : 0;
      parts[n++] = (uint64_t(uint32_t(prim_type)) << 32) | index_count;
      parts[n++] = index_buffer_info ? (uint64_t(index_buffer_info->guest_base) << 8) |
                                           uint32_t(index_buffer_info->format)
                                     : 0;
      // The float constants each shader reads, by value. Offsets would not do
      // - a block lands at a new place in the ring whenever another draw's
      // constants were placed in between - and neither would whole blocks:
      // registers the shader never reads keep what the draw before left.
      auto used_hash = [](HostShader* shader, const uint8_t* file) -> uint64_t {
        if (!shader) return 0;
        if (!shader->bool_mask_decoded) {
          rex::string::StringBuffer disassembly;
          shader->AnalyzeUcode(disassembly);
          std::memcpy(shader->bool_mask, shader->constant_register_map().bool_bitmap, sizeof(shader->bool_mask));
          shader->bool_mask_decoded = true;
        }
        const auto& map = shader->constant_register_map();
        uint64_t h = 0;
        for (uint32_t block = 0; block < 4; ++block) {
          for (uint64_t bits = map.float_bitmap[block]; bits; bits &= bits - 1) {
            const uint32_t index = block * 64 + uint32_t(__builtin_ctzll(bits));
            h = XXH3_64bits_withSeed(file + index * 16, 16, h);
          }
        }
        return h;
      };
      parts[n++] = used_hash(vs, vs_shadow_);
      parts[n++] = used_hash(ps, ps_shadow_);
      uint64_t sig = XXH3_64bits(parts, n * sizeof(uint64_t));
      // The RB/PA/SC/VGT registers (blend, depth, targets, viewport, scissor,
      // window offset) and the fetch constants (textures, vertex streams).
      sig = XXH3_64bits_withSeed(&regs.values[0x2000], 1024 * sizeof(uint32_t), sig);
      sig = XXH3_64bits_withSeed(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 192 * sizeof(uint32_t), sig);
      bool twin = false;
      for (const TwinEntry& e : twin_ring_) {
        if (e.frame == swaps_ && e.sig == sig && (e.mask & tiles) == 0) {
          twin = true;
          break;
        }
      }
      if (twin) {
        ++stats_.twin_skipped;
        Trace("twin of an earlier draw under mask {:X}, skipped", uint64_t(bin_mask_));
        return true;
      }
      twin_ring_[twin_pos_++ % twin_ring_.size()] = {sig, tiles, swaps_};
    }
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  RenderRect scissor_rect(int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
                          int32_t(scissor.offset[0] + scissor.extent[0]),
                          int32_t(scissor.offset[1] + scissor.extent[1]));
  // Split mode 5 only (see draw_bands): keep a band-predicated packet inside
  // its band.
  if (scissor_rect.bottom >= 720) {
    if (draw_bands == 0x3) {
      scissor_rect.bottom = std::min(scissor_rect.bottom, int32_t(kBandSplit));
      ++stats_.band_widened;
    } else if (draw_bands == 0xC) {
      scissor_rect.top = std::max(scissor_rect.top, int32_t(kBandSplit));
    }
  }
  if (trace_) {
    Trace("  viewport ({}, {}) {}x{} window ({}, {}) scissor ({}, {})-({}, {}) vte={:X}", viewport.x,
          viewport.y, viewport.width, viewport.height, window_x, window_y, scissor_rect.left,
          scissor_rect.top, scissor_rect.right, scissor_rect.bottom, vte.value);
    Trace("  zviewport scale={} offset={}", regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
  }
  if (scissor_rect.right <= scissor_rect.left || scissor_rect.bottom <= scissor_rect.top) {
    return Drop("empty scissor", vs, ps);
  }

  // Pipeline.
  const auto raster = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const bool polygonal = draw_util::IsPrimitivePolygonal(false, prim_type);
  RenderCullMode cull = RenderCullMode::NONE;
  if (polygonal && !(skip_mask_ & 131072)) {
    if (raster.cull_front && raster.cull_back) return Drop("culls both faces", vs, ps);
    if (raster.cull_front) cull = RenderCullMode::FRONT;
    else if (raster.cull_back) cull = RenderCullMode::BACK;
  }
  const RenderFrontFace front_face = raster.face ? RenderFrontFace::CLOCKWISE
                                                : RenderFrontFace::COUNTER_CLOCKWISE;
  const bool debug_draw =
      debug_shader_ && (vs->entry->hash == debug_shader_ || (ps && ps->entry->hash == debug_shader_));
  if (debug_draw && (debug_mode_ & 4)) {
    cull = RenderCullMode::NONE;
    alpha_test = false;
  }
  const bool depth_enabled =
      depth && depth_control.z_enable && !(skip_mask_ & 1024) && !(debug_draw && (debug_mode_ & 1));
  const bool depth_write = depth_enabled && depth_control.z_write_enable;
  // Polygon offset, converted like the Xenia Vulkan path does: the constant is
  // a fraction of the depth range, times 2^24 for a float depth target (Vulkan
  // counts it in units of the smallest depth step), the slope is in 1/16 of a
  // pixel. The guest's depth is the one stored (reversed through the viewport),
  // so a positive offset moves toward the camera here too.
  int32_t depth_bias = 0;
  float depth_slope = 0.0f;
  if (poly_offset_ && depth_enabled) {
    float slope_scale = 0.0f, offset = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope_scale, offset);
    const float bias = offset * 16777216.0f * poly_offset_scale_;
    if (std::isfinite(bias)) depth_bias = int32_t(std::clamp(bias, -2.0e9f, 2.0e9f));
    const float slope = slope_scale * xenos::kPolygonOffsetScaleSubpixelUnit * poly_offset_scale_;
    if (std::isfinite(slope)) depth_slope = slope;
  }
  // Stencil: the game marks pixels in the stencil buffer and later draws shade
  // only where they are marked; without the test those draws covered their
  // whole volume. Plume takes one reference and one pair of masks for both
  // faces, so the back face shares the front's.
  const bool stencil_enabled = depth && depth_control.stencil_enable;
  const auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
  auto stencil_face = [](xenos::CompareFunction func, xenos::StencilOp fail, xenos::StencilOp zpass,
                         xenos::StencilOp zfail) {
    // RenderStencilOp lists the Xenos operations in order, after UNKNOWN.
    auto op = [](xenos::StencilOp o) { return RenderStencilOp(uint32_t(o) + 1); };
    RenderStencilFaceDesc face;
    face.compareFunction = Compare(func);
    face.failOp = op(fail);
    face.passOp = op(zpass);
    face.depthFailOp = op(zfail);
    return face;
  };
  RenderStencilFaceDesc stencil_front, stencil_back;
  if (stencil_enabled) {
    stencil_front = stencil_face(depth_control.stencilfunc, depth_control.stencilfail,
                                 depth_control.stencilzpass, depth_control.stencilzfail);
    stencil_back = depth_control.backface_enable
                       ? stencil_face(depth_control.stencilfunc_bf, depth_control.stencilfail_bf,
                                      depth_control.stencilzpass_bf, depth_control.stencilzfail_bf)
                       : stencil_front;
  }
  RenderBlendDesc blends[4];
  for (uint32_t i = 0; i < color_count; ++i) {
    reg::RB_BLENDCONTROL bc;
    bc.value = regs[kBlendControl[i]];
    RenderBlendDesc& b = blends[i];
    b.srcBlend = Blend(bc.color_srcblend);
    b.dstBlend = Blend(bc.color_destblend);
    b.blendOp = BlendOp(bc.color_comb_fcn);
    b.srcBlendAlpha = Blend(bc.alpha_srcblend);
    b.dstBlendAlpha = Blend(bc.alpha_destblend);
    b.blendOpAlpha = BlendOp(bc.alpha_comb_fcn);
    b.blendEnabled = IsBlendable(colors[i]->format) &&
                     !(b.srcBlend == RenderBlend::ONE && b.dstBlend == RenderBlend::ZERO &&
                       b.blendOp == RenderBlendOperation::ADD &&
                       b.srcBlendAlpha == RenderBlend::ONE && b.dstBlendAlpha == RenderBlend::ZERO &&
                       b.blendOpAlpha == RenderBlendOperation::ADD);
    b.renderTargetWriteMask = uint8_t((color_mask >> (4 * i)) & 0xF);
    if (debug_draw && (debug_mode_ & 2)) {
      b.blendEnabled = false;
      b.renderTargetWriteMask = 0xF;
    }
    // renut_plume_no_bloom: the bright pass (scene * its alpha + glow) writes
    // black, so every blur after it and the final add are black too.
    if (no_bloom_ && ps && ps->entry->hash == 0x1B958C7DC860BBBEull && IsBlendable(colors[i]->format)) {
      b.blendEnabled = true;
      b.srcBlend = b.dstBlend = b.srcBlendAlpha = b.dstBlendAlpha = RenderBlend::ZERO;
      b.blendOp = b.blendOpAlpha = RenderBlendOperation::ADD;
    }
  }
  // Blend constants (RB_BLEND_RED..ALPHA) are dynamic state, set per draw, in
  // the pipelines whose factors use them. Plume left them at zero, so the
  // game's glow draws - wand sparkles, holograms - added no alpha for bloom.
  bool blend_constants = false;
  for (uint32_t i = 0; i < color_count; ++i) {
    const RenderBlendDesc& b = blends[i];
    auto constant = [](RenderBlend f) { return f == RenderBlend::BLEND_FACTOR || f == RenderBlend::INV_BLEND_FACTOR; };
    blend_constants |= b.blendEnabled && (constant(b.srcBlend) || constant(b.dstBlend) ||
                                          constant(b.srcBlendAlpha) || constant(b.dstBlendAlpha));
  }
  const uint32_t spec = (alpha_test ? SPEC_CONSTANT_ALPHA_TEST : 0) |
                        (r11g11b10_normals ? SPEC_CONSTANT_R11G11B10_NORMAL : 0);
  // Specialization constants (shader_common.h): 0 the flags above, 1 packed
  // inputs, 2 integer inputs, 3 user clip planes, 4..10 swapped inputs by usage,
  // 16 + location each input's remap. Zero is every constant's default, so
  // only the others are listed - and all of them go into the key.
  std::vector<RenderSpecConstant>& spec_constants = draw_spec_constants_;
  spec_constants.clear();
  spec_constants.emplace_back(0, spec);
  if (packed_inputs) spec_constants.emplace_back(1, packed_inputs);
  if (int_inputs) spec_constants.emplace_back(2, int_inputs);
  if (ucp_mask) spec_constants.emplace_back(3, ucp_mask);
  for (uint32_t i = 0; i < 7; ++i) {
    if (swapped[i]) spec_constants.emplace_back(4 + i, swapped[i]);
  }
  for (uint32_t i = 0; i < RENUT_SPEC_INPUT_REMAP_LOCATIONS; ++i) {
    if (input_remap[i]) spec_constants.emplace_back(RENUT_SPEC_INPUT_REMAP_BASE + i, input_remap[i]);
  }
  // The boolean registers the two shaders branch on, and only those: a
  // register neither tests stays out of the key, so it cannot multiply the
  // pipelines of shaders that ignore it.
  auto bools_of = [](HostShader* shader) -> const uint32_t* {
    if (!shader) return nullptr;
    if (!shader->bool_mask_decoded) {
      rex::string::StringBuffer disassembly;
      shader->AnalyzeUcode(disassembly);
      std::memcpy(shader->bool_mask, shader->constant_register_map().bool_bitmap,
                  sizeof(shader->bool_mask));
      shader->bool_mask_decoded = true;
    }
    return shader->bool_mask;
  };
  // A shader that reads them from the shared block adds none.
  const CompiledShader* vs_module = patch_list ? vs->domain_compiled : vs->compiled;
  const uint32_t* vs_bools = vs_module && vs_module->dynamic_bools ? nullptr : bools_of(vs);
  const uint32_t* ps_bools = ps && ps->compiled && ps->compiled->dynamic_bools ? nullptr : bools_of(ps);
  for (uint32_t word = 0; word < 8 && (vs_bools || ps_bools); ++word) {
    const uint32_t tested = (vs_bools ? vs_bools[word] : 0) | (ps_bools ? ps_bools[word] : 0);
    const uint32_t value = regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + word] & tested;
    if (value) spec_constants.emplace_back(RENUT_SPEC_BOOLS_BASE + word, value);
  }
  // The live vertex fetch table, if the shader's module has a form that reads
  // it from specialization constants (spirv_rewrites.h): what the table holds
  // comes from the shader's own vfetch instructions, so it is the same on every
  // draw that uses this shader and adds no pipelines of its own.
  CompiledShader* vertex_compiled = patch_list ? vs->domain_compiled : vs->compiled;
  const bool fetch_spec = fetch_spec_mode_ && vertex_compiled && vertex_compiled->fetch_spec_module;
  // The table is the same on every draw of the shader. With key reuse the key
  // takes one hash of it (made once, kept on the shader) and the constants
  // themselves are laid into spec_constants only when a pipeline has to be
  // made - at the position they had, so the driver's cache sees the same list.
  const auto& fetch_table = patch_list ? vs->domain_live_fetches : vs->live_fetches;
  uint64_t fetch_table_hash = 0;
  if (fetch_spec) {
    if (key_reuse_) {
      uint64_t& cached = vs->fetch_spec_hash[patch_list ? 1 : 0];
      if (!cached) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (const auto& [slot, entry] : fetch_table) {
          for (uint32_t word = 0; word < 4; ++word) {
            if (entry[word]) {
              const uint32_t pair[2] = {kFetchSpecBase + slot * 4 + word, entry[word]};
              h = XXH3_64bits_withSeed(pair, sizeof(pair), h);
            }
          }
        }
        cached = h | 1;  // never 0: that is "not made"
      }
      fetch_table_hash = cached;
    } else {
      for (const auto& [slot, entry] : fetch_table) {
        for (uint32_t word = 0; word < 4; ++word) {
          if (entry[word]) spec_constants.emplace_back(kFetchSpecBase + slot * 4 + word, entry[word]);
        }
      }
    }
  }
  if (rel_active) spec_constants.emplace_back(RENUT_SPEC_REL_CONSTANTS, rel_variant);
  // The pipeline key: the fields laid end to end and hashed once. Hashing
  // each field on its own was a few dozen XXH3 calls a draw.
  KeyBytes key_bytes;
  key_bytes.Put(vs->compiled);
  key_bytes.Put(ps ? ps->compiled : nullptr);
  key_bytes.Put(topology);
  key_bytes.Put(uint8_t(rect_list | (point_list << 1) | (patch_list << 2)));
  key_bytes.Put(cull);
  key_bytes.Put(front_face);
  key_bytes.Put(color_count);
  for (uint32_t i = 0; i < color_count; ++i) {
    key_bytes.Put(colors[i]->format);
    key_bytes.Put(blends[i].blendEnabled);
    key_bytes.Put(blends[i].srcBlend);
    key_bytes.Put(blends[i].dstBlend);
    key_bytes.Put(blends[i].blendOp);
    key_bytes.Put(blends[i].srcBlendAlpha);
    key_bytes.Put(blends[i].dstBlendAlpha);
    key_bytes.Put(blends[i].blendOpAlpha);
    key_bytes.Put(blends[i].renderTargetWriteMask);
  }
  key_bytes.Put(uint8_t((depth != nullptr) | (depth_enabled << 1) | (depth_write << 2)));
  key_bytes.Put(depth_control.zfunc);
  // Only stencil draws add bytes, so the cached pipelines of every other draw
  // keep their keys. The reference and masks are dynamic: the game changes
  // them from draw to draw, and keying on them made thousands of pipelines.
  if (stencil_enabled) key_bytes.Put(uint32_t(depth_control.value & 0xFFFFFF81u));
  // Likewise only biased draws add bytes.
  if (depth_bias != 0 || depth_slope != 0.0f) {
    key_bytes.Put(depth_bias);
    key_bytes.Put(depth_slope);
  }
  // Where the boolean registers' values sit in the key, so the family key can
  // blank them.
  std::array<size_t, 8> bool_spans;
  uint32_t bool_span_count = 0;
  for (const RenderSpecConstant& c : spec_constants) {
    key_bytes.Put(c.index);
    if (c.index >= RENUT_SPEC_BOOLS_BASE && c.index < RENUT_SPEC_BOOLS_BASE + 8 && bool_span_count < 8) {
      bool_spans[bool_span_count++] = key_bytes.size;
    }
    key_bytes.Put(c.value);
  }
  if (fetch_table_hash) key_bytes.Put(fetch_table_hash);
  // Geometry-shader and patch draws keep their own: their stages expect the
  // guest's outputs.
  const bool plain_draw = !rect_list && !point_list && !patch_list;
  // A draw with no fragment shader reads none of the vertex shader's outputs
  // but its position, so it can have the form that does not write them
  // (spirv_rewrites.h).
  RenderShader* depth_module = nullptr;
  if (depth_strip_mode_ && !ps && plain_draw && vertex_compiled) {
    depth_module = fetch_spec ? vertex_compiled->depth_fetch_spec_module.get() : vertex_compiled->depth_module.get();
  }
  const bool depth_strip = depth_module != nullptr;
  key_bytes.Put(uint8_t(fetch_spec | (depth_strip << 1)));
  for (const auto& e : input_elements) {
    key_bytes.Put(e.location);
    key_bytes.Put(e.format);
    key_bytes.Put(e.slotIndex);
    key_bytes.Put(e.alignedByteOffset);
  }
  for (const auto& s : input_slots) {
    key_bytes.Put(s.index);
    key_bytes.Put(s.stride);
  }
  const uint64_t key = key_bytes.Hash();
  // What a pipeline must agree on with the draw for another one to stand in
  // for it without faulting: attachments, primitive and vertex input. Only the
  // plume-skip bit below uses it.
  uint64_t interface_key = 0;
  if (skip_mask_ & 262144) {
    HashCombine(interface_key, topology);
    HashCombine(interface_key, rect_list);
    HashCombine(interface_key, point_list);
    HashCombine(interface_key, patch_list);
    HashCombine(interface_key, color_count);
    for (uint32_t i = 0; i < color_count; ++i) HashCombine(interface_key, colors[i]->format);
    HashCombine(interface_key, depth ? depth->format : RenderFormat::UNKNOWN);
    for (const auto& e : input_elements) {
      HashCombine(interface_key, e.location);
      HashCombine(interface_key, e.format);
      HashCombine(interface_key, e.slotIndex);
      HashCombine(interface_key, e.alignedByteOffset);
    }
    for (const auto& s : input_slots) {
      HashCombine(interface_key, s.index);
      HashCombine(interface_key, s.stride);
    }
  }
  std::unique_ptr<RenderPipeline>* pipeline_node = pipeline_memo_.Find(key);
  if (!pipeline_node) {
    pipeline_node = &pipelines_[key];
    pipeline_memo_.Put(key, pipeline_node);
  }
  auto& pipeline = *pipeline_node;
  RenderPipeline* standin = nullptr;
  if (!pipeline) {
    if (fetch_table_hash) {
      // The table the key stood for, ahead of the relative-constants entry where
      // the list had it.
      auto at = spec_constants.end() - (rel_active ? 1 : 0);
      std::vector<RenderSpecConstant> table;
      for (const auto& [slot, entry] : fetch_table) {
        for (uint32_t word = 0; word < 4; ++word) {
          if (entry[word]) table.emplace_back(kFetchSpecBase + slot * 4 + word, entry[word]);
        }
      }
      spec_constants.insert(at, table.begin(), table.end());
    }
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = depth_strip ? depth_module
                        : fetch_spec ? vertex_compiled->fetch_spec_module.get()
                                     : vertex_compiled->module.get();
    desc.geometryShader = rect_list ? rect_list_gs_.get() : point_list ? point_list_gs_.get() : nullptr;
    desc.pixelShader = ps ? ps->compiled->module.get() : nullptr;
    desc.primitiveTopology = topology;
    desc.cullMode = cull;
    desc.frontFace = front_face;
    desc.depthClipEnabled = true;
    desc.depthEnabled = depth_enabled;
    desc.depthWriteEnabled = depth_write;
    desc.depthFunction = depth_enabled ? Compare(depth_control.zfunc) : RenderComparisonFunction::ALWAYS;
    desc.depthTargetFormat = depth ? depth->format : RenderFormat::UNKNOWN;
    desc.depthBias = depth_bias;
    desc.slopeScaledDepthBias = depth_slope;
    desc.stencilEnabled = stencil_enabled;
    if (stencil_enabled) {
      desc.stencilFrontFace = stencil_front;
      desc.stencilBackFace = stencil_back;
    }
    desc.renderTargetCount = color_count;
    for (uint32_t i = 0; i < color_count; ++i) {
      desc.renderTargetFormat[i] = colors[i]->format;
      desc.renderTargetBlend[i] = blends[i];
    }
    desc.inputElements = input_elements.data();
    desc.inputElementsCount = uint32_t(input_elements.size());
    desc.inputSlots = input_slots.data();
    desc.inputSlotsCount = uint32_t(input_slots.size());
    desc.specConstants = spec_constants.data();
    desc.specConstantsCount = uint32_t(spec_constants.size());
    // The pipeline built ahead from the recipe file (StartRecipes), if this
    // start had it; a pipeline not in the file yet is written to it.
    if (recipes_ && !pending_pipelines_.count(key) &&
        !failed_pipelines_.count(key)) {
      Recipe recipe;
      recipe.vs = (patch_list ? vs->domain_entry : vs->entry)->hash;
      recipe.ps = ps ? ps->entry->hash : 0;
      recipe.domain = patch_list;
      recipe.form = depth_strip ? (fetch_spec ? kRecipeDepthFetchSpec : kRecipeDepth)
                                : (fetch_spec ? kRecipeFetchSpec : kRecipeModule);
      recipe.gs = rect_list ? 1 : point_list ? 2 : 0;
      recipe.desc = desc;
      recipe.elements = input_elements;
      recipe.slots = input_slots;
      recipe.constants = spec_constants;
      pipeline = TakeRecipe(recipe, key);
      if (pipeline) {
        ++stats_.pipelines;
        ++stats_.pipelines_prebuilt;
      }
    }
    // Exporting shaders feed later draws and sometimes the guest CPU. Never
    // skip those draws merely because their first pipeline is still compiling.
    if (!pipeline && REXCVAR_GET(renut_plume_async_pipelines) &&
        !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) {
      // A draw whose pipeline is still compiling is drawn with a ready sibling
      // (same shaders, targets, blending and vertex layout; other boolean
      // registers) when there is one. Dropping it instead left holes in the
      // scene for as long as the compile took, and the screen flickered.
      auto use_standin = [&](uint64_t family) {
        auto found = standins_.find(family);
        if (found == standins_.end()) return false;
        standin = found->second;
        ++drops_[{"pipeline stand-in", vs->entry->hash, ps ? ps->entry->hash : 0, false}];
        return true;
      };
      // A draw that goes without its pipeline says so to the job, which
      // decides what the workers take first (TakeUrgentJob): more for a
      // colour draw than a shadow caster, and more for a large mesh than a
      // particle, so walls and floors come before the effects around them.
      auto missed = [&](PipelineJob& job) {
        const uint32_t weight = (ps ? 4u : 1u) * (1u + std::min<uint32_t>(index_count, 65535u) / 512u);
        job.demand.fetch_add(weight, std::memory_order_relaxed);
        job.wanted_frame.store(swaps_, std::memory_order_relaxed);
      };
      auto pending = pending_pipelines_.find(key);
      if (pending != pending_pipelines_.end()) {
        if (!pending->second->done.load(std::memory_order_acquire)) {
          missed(*pending->second);
          if (!use_standin(pending->second->family)) return Drop("pipeline compiling", vs, ps);
        } else {
          CountJob(*pending->second);
          pipeline = std::move(pending->second->result);
          if (pipeline) standins_[pending->second->family] = pipeline.get();
          pending_pipelines_.erase(pending);
          if (!pipeline) {
            failed_pipelines_.insert(key);
            REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})",
                        vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0);
          }
        }
      } else if (!failed_pipelines_.count(key)) {
        KeyBytes family_bytes = key_bytes;
        for (uint32_t i = 0; i < bool_span_count; ++i) {
          std::memset(family_bytes.data + bool_spans[i], 0, sizeof(RenderSpecConstant::value));
        }
        const uint64_t family = family_bytes.Hash();
        const uint64_t program = ProgramKey(desc);
        // Most new pipelines differ from one already built only in blending,
        // depth, targets or vertex layout, which the driver does not compile
        // for: their program is in its cache - from this run or an earlier one
        // - and a lookup only links them. Asked here, within a budget per
        // frame, the draw is drawn now instead of a frame or more later. Not
        // for a program a worker has yet to finish: the cache cannot have it.
        bool looked_up = false;
        if (lookup_here_ && lookup_spent_us_ < lookup_budget_us_ && !pending_programs_.count(program)) {
          const auto lookup_start = std::chrono::steady_clock::now();
          {
            PlumePipelineLookupOnly only;
            pipeline = CreateGraphicsPipeline(device_, desc);
          }
          const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - lookup_start).count());
          lookup_spent_us_ += us;
          stats_.lookup_us += us;
          looked_up = true;
          if (pipeline) {
            ++stats_.pipelines;
            ++stats_.pipelines_found_here;
            stats_.pipeline_us += us;
            standins_[family] = pipeline.get();
            // A lookup that takes a compile's time is a driver that compiled
            // anyway; on this thread that is the stall the workers exist for.
            if (us > 40000 && swaps_ > 300 && ++slow_lookups_ >= 5) {
              lookup_here_ = false;
              REXLOG_WARN("Renut: Plume pipeline lookups at the draw compiled instead; left to the workers");
            }
          } else {
            ++stats_.lookups_missed;
          }
        }
        if (!pipeline) {
          // Past the limit the draw waits for a free slot rather than compiling
          // here: a stall on the command list thread is what this option avoids.
          if (pending_pipelines_.size() >= 4096) return Drop("pipeline queue full", vs, ps);
          auto job = std::make_shared<PipelineJob>();
          job->desc = desc;
          job->elements = input_elements;
          job->slots = input_slots;
          job->constants = spec_constants;
          job->family = family;
          job->program = program;
          job->looked_up = looked_up;
          job->vs_hash = vs->ucode_data_hash();
          job->ps_hash = ps ? ps->ucode_data_hash() : 0;
          const bool covered = use_standin(family);
          job->urgent = !covered;
          missed(*job);
          StartPipelineWorker();
          ++pending_programs_[program];
          pending_pipelines_.emplace(key, job);
          {
            std::lock_guard lock(pipeline_mutex_);
            (!looked_up && lookup_on_workers_ ? pipeline_lookup_queue_
             : covered                        ? pipeline_queue_
                                              : pipeline_urgent_queue_).push_back(std::move(job));
          }
          pipeline_ready_.notify_all();
          if (!covered) return Drop("pipeline compiling", vs, ps);
        }
      }
    }
    if (!pipeline && !standin && !failed_pipelines_.count(key) && !pending_pipelines_.count(key)) {
      const auto pipeline_start = std::chrono::steady_clock::now();
      pipeline = CreateGraphicsPipeline(device_, desc);
      stats_.pipeline_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - pipeline_start)
                                         .count());
      ++stats_.pipelines;
    }
    if (pipeline && getenv("RENUT_PIPELINE_STATS")) LogPipelineStatistics(*pipeline, vs, ps);
    if (pipeline && getenv("RENUT_PIPELINE_IR")) {
      DumpPipelineInternals(*pipeline, vs, ps, miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')));
    }
    if (!pipeline && !standin) {
      REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})",
                  vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0);
    }
  }
  if (!pipeline && !standin) {
    ++stats_.bad_shader;
    return true;
  }
  if (recipes_ && pipeline) NoteSceneUse(key);

  // Indices.
  uint32_t vertex_count = index_count;
  bool indexed = index_buffer_info && index_buffer_info->guest_base;
  RenderFormat index_format = RenderFormat::R16_UINT;
  uint64_t index_offset = 0;
  RenderBuffer* index_buffer = upload_.get();
  uint32_t index_bytes = 0;
  const PatchGrid* patch_grid = nullptr;
  AdaptiveDraw adaptive_draw;
  bool adaptive = false;
  if (patch_list) {
    // Every three control points are a patch - four for a quad patch - and
    // each is expanded into a grid of level * level triangles over the
    // triangle's own coordinates, or level * level quads over the quad's (see
    // GetPatchGrid). The guest's per-edge factors live where the indices would
    // be, computed by a draw that exports them to memory; the level is still
    // the ceiling the guest set for this draw.
    const uint32_t patches = index_count / (quad_patch ? 4 : 3);
    const float wanted = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    uint32_t level = std::clamp(uint32_t(wanted > 1.0f ? wanted : 1.0f), 1u, kMaxPatchLevel);
    // files/plume-patchlevel: a share of the guest's level, in percent. The
    // grid has level^2 cells, so half the level is a quarter of the vertices.
    if (patch_level_percent_ < 100) level = std::max(1u, level * patch_level_percent_ / 100);
    indexed = false;
    if (!patches) {
      ++stats_.skipped_prim;
      return true;
    }
    // In adaptive mode the "indices" are the factors, three per triangle
    // patch, which the guest exported just before: those patches are cut per
    // edge on the GPU, most of them - all that are off screen - into a single
    // triangle. Quad patches stay on the grid.
    if (adaptive_mode_ && !quad_patch &&
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive && index_buffer_info &&
        index_buffer_info->guest_base && index_buffer_info->format == xenos::IndexFormat::kInt32) {
      const float least = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
      const uint32_t min_level = std::clamp(uint32_t(least > 1.0f ? least : 1.0f), 1u, level);
      adaptive = ExpandAdaptivePatches(index_buffer_info->guest_base, index_buffer_info->endianness, patches,
                                       min_level, level, adaptive_draw);
    }
    if (!adaptive) {
      patch_grid = GetPatchGrid(quad_patch, level, patches);
      if (!patch_grid) return Drop("patch grid", vs, ps);
      vertex_count = patches * patch_grid->indices_per_patch;
    } else {
      vertex_count = 0;
    }
  }
  if (indexed) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    index_format = is32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT;
    const uint32_t size = index_count * (is32 ? 4 : 2);
    // A quad list's indices are read back here to be split into triangles,
    // so they stay in the ring.
    if (native_swapped_) stream_object_ = native_indices_;
    const bool uploaded =
        quad_list ? UploadGuest(index_buffer_info->guest_base, size, index_buffer_info->endianness, index_offset)
                  : GuestStream(index_buffer_info->guest_base, size, index_buffer_info->endianness,
                                index_buffer, index_offset);
    stream_object_ = {};
    if (!uploaded) return Drop("index upload", vs, ps);
    if (validate_windows_ && !quad_list) {
      ValidateRange("index", vs->entry->hash, index_buffer, index_offset, index_buffer_info->guest_base, size,
                    index_buffer_info->endianness);
    }
    index_bytes = size;
  }
  if (quad_list) {
    // Two triangles per quad, as 32-bit indices.
    const uint32_t quads = index_count / 4;
    uint64_t offset = Allocate(uint64_t(quads) * 6 * 4, 256);
    if (offset == UINT64_MAX) return Drop("upload full", vs, ps);
    auto* out = reinterpret_cast<uint32_t*>(Pointer(offset));
    for (uint32_t q = 0; q < quads; ++q) {
      uint32_t v[4];
      for (uint32_t k = 0; k < 4; ++k) {
        uint32_t i = q * 4 + k;
        if (!indexed) v[k] = i;
        else if (index_format == RenderFormat::R32_UINT)
          v[k] = reinterpret_cast<const uint32_t*>(Pointer(index_offset))[i];
        else
          v[k] = reinterpret_cast<const uint16_t*>(Pointer(index_offset))[i];
      }
      const uint32_t tri[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
      std::memcpy(out + q * 6, tri, sizeof(tri));
    }
    indexed = true;
    index_format = RenderFormat::R32_UINT;
    index_buffer = upload_.get();
    index_offset = offset;
    index_bytes = quads * 6 * 4;
    vertex_count = quads * 6;
  }

  // Record. What the Vulkan calls need goes into a packet first. A draw that
  // cannot change the picture when it is reordered joins a window and is
  // recorded, grouped by pipeline, when the window closes; any other draw
  // closes the window and is recorded at once. Nothing from here on reads the
  // register file for the draw's sake, so it can wait.
  RenderFramebuffer* framebuffer = GetFramebuffer(colors, color_count, depth);
  if (!framebuffer) {
    ++stats_.no_target;
    return true;
  }
  DrawPacket pk;
  pk.framebuffer = framebuffer;
  pk.pass_colors = color_count_of_pass_;
  pk.pass_depth = depth_of_pass_;
  pk.pipeline = standin ? standin : pipeline.get();
  pk.interface_key = interface_key;
  pk.vs_hash = vs->entry->hash ^ (uint64_t(rel_constants_mode_ >= 10 ? rel_variant : 0) << 60);
  pk.ps_hash = ps ? ps->entry->hash : 0;
  if (trail_) {
    pk.s0_addr = streams[0];
    pk.s0_size = slot_sizes[0];
    pk.s10_addr = streams[10];
    pk.s10_size = slot_sizes[10];
  }
  if (trail_) {
    for (uint32_t i = 0; i < RENUT_VERTEX_STREAM_COUNT; ++i) {
      pk.stream_addr[i] = streams[i];
      pk.stream_size[i] = slot_sizes[i];
    }
  }
  if (blend_constants) {
    pk.blend_constants = true;
    pk.blend_factor[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    pk.blend_factor[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    pk.blend_factor[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    pk.blend_factor[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }
  if (stencil_enabled) {
    pk.stencil = true;
    pk.stencil_ref = stencil_ref_mask.stencilref;
    pk.stencil_mask = stencil_ref_mask.stencilmask;
    pk.stencil_write_mask = stencil_ref_mask.stencilwritemask;
  }
  // Inactive fragment bindings still need a valid offset. Keep their values
  // uncommitted so the next draw with a fragment shader uploads them.
  const bool skip_ps_constants = !ps && shadow_native_constants_;
  const uint64_t unused_ps_offset = slice_base_;
  pk.push[0] = Address(constant_offsets_[0][0]);
  pk.push[1] = Address(skip_ps_constants ? unused_ps_offset : constant_offsets_[1][0]);
  pk.push[2] = Address(shared_offset_);
  for (uint32_t stage = 0; stage < 2; ++stage) {
    for (uint32_t chunk = 0; chunk < kConstantChunks; ++chunk) {
      pk.constant_offsets[stage * kConstantChunks + chunk] =
          uint32_t(stage == 1 && skip_ps_constants ? unused_ps_offset : constant_offsets_[stage][chunk]);
    }
  }
  pk.constant_offsets[kSharedHotBinding] = uint32_t(shared_offset_);
  pk.constant_offsets[kSharedColdBinding] = uint32_t(shared_cold_offset_);
  // Draws that read no constant by a0/aL keep whatever whole-file block was
  // placed last (any valid offset will do, nothing reads it), so they do not
  // rebind the set.
  pk.constant_offsets[kRelBinding] = pk.constant_offsets[kRelBinding + 1] =
      uint32_t(vs_all_offset_ != UINT64_MAX ? vs_all_offset_ : unused_ps_offset);
  pk.viewport = viewport;
  // plume-skip bit 8388608 scissors every draw to one pixel: vertex work and
  // binning stay, fragment work goes. What is left is the per-draw cost.
  if (skip_mask_ & 8388608) scissor_rect = RenderRect(0, 0, 1, 1);
  // A shadow map drawn at a fraction of the guest's resolution (see Target).
  if (extent && extent->scale != 4) {
    const float fraction = float(extent->scale) / 4.f;
    pk.viewport.x *= fraction;
    pk.viewport.y *= fraction;
    pk.viewport.width *= fraction;
    pk.viewport.height *= fraction;
    auto low = [&](int32_t v) { return int32_t(HostFloor(extent, uint32_t(std::max(v, 0)))); };
    auto high = [&](int32_t v) { return int32_t(HostCeil(extent, uint32_t(std::max(v, 0)))); };
    scissor_rect = RenderRect(std::min(low(scissor_rect.left), int32_t(extent->width)),
                              std::min(low(scissor_rect.top), int32_t(extent->height)),
                              std::min(high(scissor_rect.right), int32_t(extent->width)),
                              std::min(high(scissor_rect.bottom), int32_t(extent->height)));
  }
  pk.scissor = scissor_rect;
  draw_slots_.clear();
  for (const auto& slot : input_slots) {
    DeferredSlot entry;
    entry.slot = slot;
    if (adaptive) {
      entry.view = RenderVertexBufferView(RenderBufferReference(adaptive_draw.buffer, adaptive_draw.points_offset),
                                          uint32_t(adaptive_draw.points_bytes));
    } else if (patch_list) {
      entry.view = RenderVertexBufferView(RenderBufferReference(patch_grid->points.get(), 0),
                                          patch_grid->patches * patch_grid->points_per_patch * 4 *
                                              uint32_t(sizeof(float)));
    } else if (slot.index == kZeroBinding) {
      entry.view = RenderVertexBufferView(RenderBufferReference(upload_.get(), zero_offset_), 64);
    } else {
      entry.view = RenderVertexBufferView(RenderBufferReference(slot_buffers[slot.index], slot_offsets[slot.index]),
                                          slot_sizes[slot.index]);
    }
    draw_slots_.push_back(entry);
  }
  pk.slot_count = uint32_t(draw_slots_.size());
  const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
  if (trace_ && index_buffer == upload_.get()) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < vertex_count; ++i) {
      uint32_t v = !indexed ? i
                   : index_format == RenderFormat::R32_UINT
                       ? reinterpret_cast<const uint32_t*>(Pointer(index_offset))[i]
                       : reinterpret_cast<const uint16_t*>(Pointer(index_offset))[i];
      if (indexed && v == (index_format == RenderFormat::R32_UINT ? 0xFFFFFFFFu : 0xFFFFu)) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    std::string line = fmt::format("  indices {}..{} base={} count={}", lo, hi, base_vertex, vertex_count);
    for (const LiveElement& e : vs->elements) {
      if (e.usage != 0 || !e.stride || !slot_sizes[e.slot] || slot_buffers[e.slot] != upload_.get()) continue;
      const uint32_t capacity = slot_sizes[e.slot] / e.stride;
      line += fmt::format(" pos(slot {} fmt {} cap {})", e.slot, e.format, capacity);
      // Decode the referenced positions as the host will read them.
      float mn = 1e30f, mx = -1e30f;
      uint32_t bad = 0;
      for (uint32_t v = lo; v <= hi && v - lo < 4096; ++v) {
        const uint64_t vi = uint64_t(int64_t(v) + base_vertex);
        const uint64_t at = vi * e.stride + e.offset;
        if (at + 16 > slot_sizes[e.slot]) { ++bad; continue; }
        const uint8_t* p = Pointer(slot_offsets[e.slot]) + at;
        for (int c = 0; c < 3; ++c) {
          float f;
          if (e.format == 32) {
            uint16_t h;
            std::memcpy(&h, p + 2 * (c ^ 1), 2);  // pairs reversed by the 32-bit swap
            const uint32_t sign = h >> 15, exp = (h >> 10) & 31, mant = h & 1023;
            f = exp == 31 ? INFINITY : exp ? std::ldexp(float(mant | 1024), int(exp) - 25)
                                           : std::ldexp(float(mant), -24);
            if (sign) f = -f;
          } else {
            std::memcpy(&f, p + 4 * c, 4);
          }
          if (!std::isfinite(f)) ++bad;
          else { mn = std::min(mn, f); mx = std::max(mx, f); }
        }
      }
      line += fmt::format(" range [{}, {}] bad={}", mn, mx, bad);
    }
    Trace("{}", line);
  }
  // plume-skip bit 1073741824 records everything but the draw itself: the
  // state, pipeline and constant changes are all that reach the GPU.
  stats_.vertices += vertex_count;
  if (rect_list || point_list) ++stats_.gs_draws;
  if (patch_list) {
    ++stats_.patch_draws;
    stats_.patch_vertices += vertex_count;
  }
  // plume-skip bit 2147483648 submits only each draw's first triangle: the
  // per-draw state and setup stay, the vertex work goes.
  const uint32_t draw_count = (skip_mask_ & 2147483648u) ? std::min(vertex_count, 3u) : vertex_count;
  if (skip_mask_ & 1073741824) {
    pk.kind = DrawPacket::Kind::kNone;
  } else if (indexed) {
    // The ring is bound whole and each draw starts at its own indices, which
    // Allocate aligns to 256 bytes - one binding for every draw of the list.
    const uint32_t index_size = index_format == RenderFormat::R32_UINT ? 4 : 2;
    pk.kind = DrawPacket::Kind::kIndexed;
    pk.index_buffer = index_buffer;
    pk.index_format = index_format;
    pk.count = draw_count;
    pk.first = uint32_t(index_offset / index_size);
    pk.base_vertex = base_vertex;
  } else if (adaptive) {
    pk.kind = DrawPacket::Kind::kPatchIndirect;
    pk.index_buffer = adaptive_draw.buffer;
    pk.index_format = RenderFormat::R32_UINT;
    pk.indirect_offset = adaptive_draw.args_offset;
  } else if (patch_list) {
    pk.kind = DrawPacket::Kind::kPatch;
    pk.index_buffer = patch_grid->indices.get();
    pk.index_format = RenderFormat::R32_UINT;
    pk.count = draw_count;
  } else {
    pk.kind = DrawPacket::Kind::kPlain;
    pk.count = draw_count;
    pk.first = uint32_t(std::max(base_vertex, 0));
  }

  // Which draws may wait. Two opaque draws that test and write depth in a
  // strict direction leave the nearer fragment at every pixel whichever one is
  // recorded first - only an exact depth tie could differ - provided they share
  // what a passing fragment writes: the attachments, the colour mask and the
  // depth and stencil state. Stencil reorders only when it never reads and
  // writes on depth pass alone, since then the surviving value is the nearest
  // surface's either way. Depth-only draws (the shadow maps) are exactly
  // independent; with mode 2 opaque colour draws join them. Draws that
  // export to memory, patches, merged bands and every debug switch stay in
  // order.
  bool defer = false;
  uint64_t window_signature = 0;
  if (defer_mode_ && !sync_draws_ && !trace_ && !skip_mask_ && !bind_skip_ && !debug_draw && !patch_list &&
      !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT) && depth_write) {
    const xenos::CompareFunction zf = depth_control.zfunc;
    const bool strict = zf == xenos::CompareFunction::kLess || zf == xenos::CompareFunction::kLessEqual ||
                        zf == xenos::CompareFunction::kGreater || zf == xenos::CompareFunction::kGreaterEqual;
    auto stencil_free = [](xenos::CompareFunction func, xenos::StencilOp fail, xenos::StencilOp zpass,
                           xenos::StencilOp zfail) {
      return func == xenos::CompareFunction::kAlways && fail == xenos::StencilOp::kKeep &&
             zfail == xenos::StencilOp::kKeep &&
             (zpass == xenos::StencilOp::kKeep || zpass == xenos::StencilOp::kZero ||
              zpass == xenos::StencilOp::kReplace);
    };
    const bool stencil_ok =
        !stencil_enabled ||
        (stencil_free(depth_control.stencilfunc, depth_control.stencilfail, depth_control.stencilzpass,
                      depth_control.stencilzfail) &&
         (!depth_control.backface_enable ||
          stencil_free(depth_control.stencilfunc_bf, depth_control.stencilfail_bf,
                       depth_control.stencilzpass_bf, depth_control.stencilzfail_bf)));
    bool opaque = true;
    for (uint32_t i = 0; i < color_count; ++i) opaque = opaque && !blends[i].blendEnabled;
    // A colour draw with a depth bias is a decal over geometry at the same depth,
    // which is exactly where order decides: it stays in order. Shadow maps use
    // a bias too, and do not care.
    const bool biased = raster.poly_offset_front_enable || raster.poly_offset_back_enable;
    defer = strict && stencil_ok && (!color_pass || (defer_mode_ >= 2 && opaque && !biased));
    // Hi-Z: tested against the depth the pass holds when the window closes. Not
    // biased draws (the bias moves their depth), and only the "closer wins"
    // functions, whose fragments pass when they are nearer.
    if (hiz_mode_ >= 3 && hiz_test_.valid) {
      // Where the boxed draws go: not deferred, biased, other depth function, no depth.
      ++stats_.hiz_boxed_draws;
      if (!defer) ++stats_.hiz_why_not[0];
      else if (biased) ++stats_.hiz_why_not[1];
      else if (!(zf == xenos::CompareFunction::kLess || zf == xenos::CompareFunction::kLessEqual)) ++stats_.hiz_why_not[2];
      else if (!depth || !depth->texture) ++stats_.hiz_why_not[3];
    }
    if (defer && hiz_mode_ >= 3 && hiz_cond_ && hiz_test_.valid && !biased && depth && depth->texture &&
        (zf == xenos::CompareFunction::kLess || zf == xenos::CompareFunction::kLessEqual) &&
        (color_pass || hiz_mode_ >= 4)) {
      const float fraction = float(depth->scale) / 4.f;
      pk.hiz_depth = depth;
      pk.hiz_texture = depth->texture.get();
      pk.hiz_rect[0] = std::clamp(hiz_test_.rect[0] * fraction, 0.f, float(depth->width));
      pk.hiz_rect[1] = std::clamp(hiz_test_.rect[1] * fraction, 0.f, float(depth->height));
      pk.hiz_rect[2] = std::clamp(hiz_test_.rect[2] * fraction, 0.f, float(depth->width));
      pk.hiz_rect[3] = std::clamp(hiz_test_.rect[3] * fraction, 0.f, float(depth->height));
      pk.hiz_z = hiz_test_.z;
      pk.hiz_key = hiz_test_.key ^ (pk.ps_hash * 0xC2B2AE3D27D4EB4Full);
      if (pk.hiz_rect[2] < pk.hiz_rect[0] || pk.hiz_rect[3] < pk.hiz_rect[1]) pk.hiz_depth = nullptr;
    }
    if (defer) {
      HashCombine(window_signature, framebuffer);
      HashCombine(window_signature, color_mask);
      HashCombine(window_signature, uint32_t(depth_control.value));
      HashCombine(window_signature, stencil_enabled ? uint32_t(stencil_ref_mask.stencilwritemask) : 0u);
      HashCombine(window_signature, uint32_t(edram_mode));
    }
  }
  if (defer) {
    if (!deferred_.empty() && deferred_signature_ != window_signature) FlushDeferred(1);
    deferred_signature_ = window_signature;
    pk.first_slot = uint32_t(deferred_slots_.size());
    deferred_slots_.insert(deferred_slots_.end(), draw_slots_.begin(), draw_slots_.end());
    deferred_.push_back(pk);
    ++stats_.deferred_draws;
    if (deferred_.size() >= (hiz_mode_ >= 3 ? kMaxDeferredHiz : kMaxDeferred)) FlushDeferred(2);
    return true;
  }
  FlushDeferred(3);
  if (RecActive()) RecQueue(pk, draw_slots_.data(), uint32_t(draw_slots_.size()));
  else EmitDraw(pk, draw_slots_.data(), uint32_t(draw_slots_.size()));
  return true;
}

void PlumeCommandProcessor::RecQueue(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count) {
  if (!rec_worker_.thread.joinable()) {
    rec_worker_.mutex = &rec_mutex_;
    rec_worker_.cv = &rec_cv_;
    rec_worker_.stop = &rec_stop_;
    rec_stop_ = false;
    rec_worker_.thread = std::thread([this] { RecThreadMain(); });
    rec_thread_id_ = rec_worker_.thread.get_id();
  }
  if (!rec_pending_) {
    {
      std::lock_guard<std::mutex> lock(rec_mutex_);
      if (!rec_free_.empty()) {
        rec_pending_ = rec_free_.back();
        rec_free_.pop_back();
      }
    }
    if (!rec_pending_) {
      rec_owned_.push_back(std::make_unique<RecJob>());
      rec_pending_ = rec_owned_.back().get();
      rec_pending_->packets.reserve(kRecBatch);
      rec_pending_->slots.reserve(kRecBatch * 8);
    }
    rec_pending_->packets.clear();
    rec_pending_->slots.clear();
  }
  RecJob& job = *rec_pending_;
  DrawPacket& copy = job.packets.emplace_back(pk);
  copy.first_slot = uint32_t(job.slots.size());
  copy.slot_count = slot_count;
  job.slots.insert(job.slots.end(), slots, slots + slot_count);
  if (job.packets.size() >= kRecBatch) RecPost();
}

void PlumeCommandProcessor::RecPost() {
  if (!rec_pending_) return;
  if (rec_pending_->packets.empty()) return;
  {
    std::lock_guard<std::mutex> lock(rec_mutex_);
    rec_queue_.push_back(rec_pending_);
    rec_queued_.fetch_add(1, std::memory_order_release);
    rec_posted_.fetch_add(1, std::memory_order_release);
  }
  rec_pending_ = nullptr;
  rec_cv_.notify_one();
}

// Waits until everything handed to the recorder has been recorded.
void PlumeCommandProcessor::SyncRecorder() {
  if (OnRecorder()) return;
  RecPost();
  const uint64_t target = rec_posted_.load(std::memory_order_acquire);
  if (rec_completed_.load(std::memory_order_acquire) >= target) return;
  for (int i = 0; i < 4000; ++i) {
    if (rec_completed_.load(std::memory_order_acquire) >= target) return;
  }
  std::unique_lock<std::mutex> lock(rec_mutex_);
  rec_done_cv_.wait(lock, [&] { return rec_completed_.load(std::memory_order_acquire) >= target; });
}

void PlumeCommandProcessor::RecThreadMain() {
  pthread_setname_np(pthread_self(), "PlumeRecord");
  for (;;) {
    RecJob* job = nullptr;
    // A short spin first: waking a sleeping thread costs more than the gap
    // between two batches of a busy frame.
    for (int i = 0; i < 20000 && !job; ++i) {
      if (rec_queued_.load(std::memory_order_acquire) == 0) continue;
      std::lock_guard<std::mutex> lock(rec_mutex_);
      if (!rec_queue_.empty()) {
        job = rec_queue_.front();
        rec_queue_.pop_front();
        rec_queued_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    if (!job) {
      std::unique_lock<std::mutex> lock(rec_mutex_);
      rec_cv_.wait(lock, [&] { return rec_stop_ || !rec_queue_.empty(); });
      if (rec_queue_.empty()) return;  // stopping
      job = rec_queue_.front();
      rec_queue_.pop_front();
      rec_queued_.fetch_sub(1, std::memory_order_relaxed);
    }
    for (const DrawPacket& pk : job->packets) {
      EmitDraw(pk, job->slots.data() + pk.first_slot, pk.slot_count);
    }
    {
      std::lock_guard<std::mutex> lock(rec_mutex_);
      rec_free_.push_back(job);
      rec_completed_.fetch_add(1, std::memory_order_release);
    }
    rec_done_cv_.notify_all();
  }
}

// Records one packet: the pass, the pipeline and every piece of per-draw state
// that differs from what the list already has bound, then the draw itself.
void PlumeCommandProcessor::EmitDraw(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count) {
  // Not from the recorder (the Hi-Z windows, the debug modes): nothing may be
  // in flight while this thread touches the bound state.
  if (!OnRecorder()) SyncRecorder();
  const bool pair_stamped = pair_capture_ && PairBegin(pk);
  if (OnRecorder()) {
    emit_colors_ = pk.pass_colors;
    emit_depth_ = pk.pass_depth;
  } else {
    color_count_of_pass_ = pk.pass_colors;
    depth_of_pass_ = pk.pass_depth;
  }
  SetFramebuffer(pk.framebuffer);
  // plume-skip bit 262144 keeps the bound pipeline whenever it can stand in
  // for the draw's own (same attachments and vertex input): the picture is
  // wrong, and what the GPU saves is the price of switching programs.
  const bool keep_pipeline = (skip_mask_ & 262144) && bound_pipeline_ && pk.interface_key == bound_interface_key_;
  if (bound_pipeline_ != pk.pipeline && !keep_pipeline) {
    L()->setPipeline(pk.pipeline);
    bound_pipeline_ = pk.pipeline;
    bound_interface_key_ = pk.interface_key;
    ++rec_stats_.pipeline_binds;
  }
  // Dynamic state of the pipelines that use it (the Vulkan layer makes it so,
  // see plume_renderer.cpp): not part of Plume's interface, so on the command
  // buffer itself.
  if (pk.blend_constants || pk.stencil) {
    const VkCommandBuffer commands = static_cast<VulkanCommandList*>(L())->vk;
    if (pk.blend_constants) vkCmdSetBlendConstants(commands, pk.blend_factor);
    if (pk.stencil) {
      vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, pk.stencil_ref);
      vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, pk.stencil_mask);
      vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, pk.stencil_write_mask);
    }
  }
  if (std::memcmp(pk.push, pushed_constants_, sizeof(pk.push)) != 0 &&
             !((bind_skip_ & 2) && pushed_constants_[0] != UINT64_MAX)) {
    L()->setGraphicsPushConstants(0, pk.push, 0, sizeof(pk.push));
    std::memcpy(pushed_constants_, pk.push, sizeof(pk.push));
  }
  if (std::memcmp(pk.constant_offsets, bound_constant_offsets_, sizeof(pk.constant_offsets)) != 0 &&
      !((bind_skip_ & 1) && bound_constant_offsets_[0] != UINT32_MAX)) {
    vkCmdBindDescriptorSets(static_cast<VulkanCommandList*>(L())->vk, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            static_cast<VulkanPipelineLayout*>(layout_.get())->vk, 4, 1, &constants_set_,
                            kConstantBindings, pk.constant_offsets);
    std::memcpy(bound_constant_offsets_, pk.constant_offsets, sizeof(pk.constant_offsets));
  }
  // Dynamic state, vertex and index buffers are only set when they change:
  // Plume passes each call straight to Vulkan, and Turnip re-emits the state
  // behind every one of them at the next draw.
  if (!viewport_bound_ ||
      (std::memcmp(&pk.viewport, &bound_viewport_, sizeof(pk.viewport)) != 0 && !(bind_skip_ & 8))) {
    L()->setViewports(&pk.viewport, 1);
    bound_viewport_ = pk.viewport;
    viewport_bound_ = true;
  }
  if (!scissor_bound_ ||
      (std::memcmp(&pk.scissor, &bound_scissor_, sizeof(pk.scissor)) != 0 && !(bind_skip_ & 8))) {
    L()->setScissors(&pk.scissor, 1);
    bound_scissor_ = pk.scissor;
    scissor_bound_ = true;
  }
  for (uint32_t i = 0; i < slot_count; ++i) {
    const RenderInputSlot& slot = slots[i].slot;
    const RenderVertexBufferView& view = slots[i].view;
    VertexBinding& bound = bound_vertex_buffers_[slot.index];
    if ((bound.buffer != view.buffer.ref || bound.offset != view.buffer.offset) &&
        !((bind_skip_ & 4) && bound.buffer)) {
      // Straight to Vulkan: Plume's wrapper goes through two thread_local
      // vectors a call, which was ~4% of this thread at a call or two a draw.
      if (const auto* vb = static_cast<const VulkanBuffer*>(view.buffer.ref)) {
        const VkDeviceSize offset = view.buffer.offset;
        vkCmdBindVertexBuffers(static_cast<VulkanCommandList*>(L())->vk, slot.index, 1, &vb->vk, &offset);
      } else {
        L()->setVertexBuffers(slot.index, &view, 1, &slot);
      }
      bound.buffer = view.buffer.ref;
      bound.offset = view.buffer.offset;
    }
  }
  // A draw the Hi-Z test answered for runs only where the answer is non-zero.
  const bool predicated = pk.hiz_pred_offset != UINT32_MAX && hiz_pred_[slot_] != nullptr &&
                          pk.kind != DrawPacket::Kind::kNone;
  VkCommandBuffer predicate_cb = VK_NULL_HANDLE;
  const bool queried = pk.hiz_query != UINT32_MAX && hiz_query_pool_ != VK_NULL_HANDLE &&
                       pk.kind != DrawPacket::Kind::kNone;
  if (queried) {
    auto* vulkan = static_cast<VulkanCommandList*>(L());
    vulkan->checkActiveRenderPass();
    predicate_cb = vulkan->vk;
    vkCmdBeginQuery(predicate_cb, hiz_query_pool_, pk.hiz_query, 0);
  }
  if (predicated) {
    auto* vulkan = static_cast<VulkanCommandList*>(L());
    vulkan->checkActiveRenderPass();
    predicate_cb = vulkan->vk;
    VkConditionalRenderingBeginInfoEXT info = {VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
    info.buffer = static_cast<VulkanBuffer*>(hiz_pred_[slot_].get())->vk;
    info.offset = pk.hiz_pred_offset;
    vkCmdBeginConditionalRenderingEXT(predicate_cb, &info);
  }
  switch (pk.kind) {
    case DrawPacket::Kind::kNone: break;
    case DrawPacket::Kind::kIndexed:
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      L()->drawIndexedInstanced(pk.count, pk.instances, pk.first, pk.base_vertex, 0);
      break;
    case DrawPacket::Kind::kPatch:
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      L()->drawIndexedInstanced(pk.count, 1, 0, 0, 0);
      break;
    case DrawPacket::Kind::kPatchIndirect: {
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      auto* vulkan = static_cast<VulkanCommandList*>(L());
      vulkan->checkActiveRenderPass();
      vkCmdDrawIndexedIndirect(vulkan->vk, static_cast<VulkanBuffer*>(pk.index_buffer)->vk, pk.indirect_offset, 1,
                               sizeof(VkDrawIndexedIndirectCommand));
      break;
    }
    case DrawPacket::Kind::kPlain: L()->drawInstanced(pk.count, 1, pk.first, 0); break;
  }
  if (predicated) {
    vkCmdEndConditionalRenderingEXT(predicate_cb);
    ++rec_stats_.predicated;
  }
  if (queried) vkCmdEndQuery(predicate_cb, hiz_query_pool_, pk.hiz_query);
  if (pair_stamped) PairStamp();
  ++rec_stats_.drawn;
  if (!pass_marks_[slot_].empty()) ++pass_marks_[slot_].back().draws;
  if (trail_) {
    // Only one draw in eight asks (a hash lookup a draw was over 1% of this
    // thread): the report of a lost device calls a pair new from its first
    // frame, and an unknown one is simply not called new.
    uint64_t first_frame = 0;
    if ((rec_stats_.drawn & 7) == 0) {
      first_frame = pair_first_frame_.try_emplace(pk.vs_hash * 1099511628211ull ^ pk.ps_hash, swaps_).first->second;
    }
    const uint64_t at = recent_draw_count_.load(std::memory_order_relaxed);
    RecentDraw& recent = recent_draws_[at % kRecentDraws];
    recent = {swaps_, pk.vs_hash, pk.ps_hash, uint64_t(uintptr_t(pk.pipeline)),
              uint64_t(uintptr_t(pk.framebuffer)), pk.s0_addr, pk.s10_addr, first_frame,
              uint32_t(pk.kind), pk.count, pk.first, pk.s0_size, pk.s10_size};
    std::memcpy(recent.stream_addr, pk.stream_addr, sizeof(recent.stream_addr));
    std::memcpy(recent.stream_size, pk.stream_size, sizeof(recent.stream_size));
    recent.base_vertex = pk.base_vertex;
    auto address_of = [](const RenderBuffer* buffer) -> uint64_t {
      const auto* vulkan = static_cast<const VulkanBuffer*>(buffer);
      return vulkan && (vulkan->desc.flags & RenderBufferFlag::DEVICE_ADDRESSABLE) ? vulkan->getDeviceAddress() : 0;
    };
    if (pk.kind == DrawPacket::Kind::kIndexed || pk.kind == DrawPacket::Kind::kPatch) {
      recent.index_size = pk.index_format == RenderFormat::R32_UINT ? 4 : 2;
      recent.index_addr = address_of(pk.index_buffer);
      recent.index_bytes = pk.count * recent.index_size;
      if (pk.kind == DrawPacket::Kind::kIndexed) recent.index_addr += uint64_t(pk.first) * recent.index_size;
    }
    for (uint32_t i = 0; i < slot_count && recent.vertex_count < 6; ++i) {
      const RenderVertexBufferView& view = slots[i].view;
      recent.vertex[recent.vertex_count++] = {address_of(view.buffer.ref) + view.buffer.offset, view.size,
                                              slots[i].slot.index};
    }
    recent_draw_count_.store(at + 1, std::memory_order_release);
  }
  if (sync_draws_) {
    SyncAfterDraw(pk);
  } else if (sync_every_ > 1 && ++sync_every_counter_ >= sync_every_) {
    sync_every_counter_ = 0;
    SyncEvery();
  }
}

// A lost device answers every later call with an error and nothing in the guest
// ever learns of it: the watchdog just keeps waiting for a fence nobody will
// signal. Names the draws of the last frames, new vertex/pixel pairs first -
// the Mali takes the device down on a draw's data or program, and the draw is
// recorded a frame or two before the submit that reports it.
void PlumeCommandProcessor::ReportDeviceLost() {
  unsigned long long avail_kb = 0;
  if (FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
    char line[128];
    while (std::fgets(line, sizeof line, meminfo)) {
      if (std::sscanf(line, "MemAvailable: %llu kB", &avail_kb) == 1) break;
    }
    std::fclose(meminfo);
  }
  const uint64_t now = swaps_;
  REXLOG_ERROR("Renut: Plume device lost at swap {}; {} MB of RAM available; {} draws recorded", now,
               avail_kb >> 10, recent_draw_count_.load());
  REXLOG_ERROR("Renut: Plume 2D descriptor writes {} (skipped {} as unchanged)", descriptor_writes_,
               descriptor_writes_skipped_);
  if (validate_windows_) {
    REXLOG_ERROR("Renut: Plume validation: {} ranges compared, {} stale", validate_ranges_, validate_stale_);
  }
  if (skin_log_) LogSkinned();
  const uint64_t total = recent_draw_count_.load(std::memory_order_acquire);
  const uint64_t begin = total > kRecentDraws ? total - kRecentDraws : 0;
  // Every draw of the last two frames with the memory it reads, for matching
  // against the fault address the kernel logs (dmesg, via adb bugreport).
  if (!miss_dump_directory_.empty()) {
    const std::string path = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/plume-lost.txt";
    if (FILE* out = std::fopen(path.c_str(), "w")) {
      std::fprintf(out, "device lost at swap %llu\n", (unsigned long long)now);
      for (uint64_t i = begin; i < total; ++i) {
        const RecentDraw& d = recent_draws_[i % kRecentDraws];
        if (d.frame + 2 < now) continue;
        std::fprintf(out, "d%llu f%llu vs=%016llX ps=%016llX kind=%u n=%u first=%u base=%d pipe=%llX fb=%llX",
                     (unsigned long long)i, (unsigned long long)d.frame, (unsigned long long)d.vs,
                     (unsigned long long)d.ps, d.kind, d.count, d.first, d.base_vertex, (unsigned long long)d.pipeline,
                     (unsigned long long)d.framebuffer);
        std::fprintf(out, " ib=%llX+%X/%u", (unsigned long long)d.index_addr, d.index_bytes, d.index_size);
        for (uint32_t v = 0; v < d.vertex_count; ++v) {
          std::fprintf(out, " vb%u=%llX+%X", d.vertex[v].slot, (unsigned long long)d.vertex[v].addr, d.vertex[v].size);
        }
        for (uint32_t st = 0; st < 17; ++st) {
          if (d.stream_addr[st]) {
            std::fprintf(out, " s%u=%llX+%X", st, (unsigned long long)d.stream_addr[st], d.stream_size[st]);
          }
        }
        std::fprintf(out, "\n");
      }
      std::fclose(out);
    }
  }
  struct Group {
    RecentDraw draw;
    uint32_t draws = 0;
    uint64_t indices = 0;
  };
  std::vector<std::pair<uint64_t, Group>> groups;
  std::unordered_map<uint64_t, size_t> index_of;
  for (uint64_t i = begin; i < total; ++i) {
    const RecentDraw& d = recent_draws_[i % kRecentDraws];
    if (d.frame + 3 < now) continue;
    uint64_t key = d.frame;
    key = key * 1099511628211ull ^ d.vs;
    key = key * 1099511628211ull ^ d.ps;
    key = key * 1099511628211ull ^ d.pipeline;
    key = key * 1099511628211ull ^ d.framebuffer;
    auto [it, fresh] = index_of.try_emplace(key, groups.size());
    if (fresh) groups.push_back({key, Group{d, 0, 0}});
    Group& group = groups[it->second].second;
    ++group.draws;
    group.indices += d.count;
  }
  // Newest pairs first, then the rest in the order they were recorded.
  std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
    return a.second.draw.pair_first_frame > b.second.draw.pair_first_frame;
  });
  size_t shown = 0;
  for (const auto& [key, group] : groups) {
    if (shown++ >= 160) break;
    const RecentDraw& d = group.draw;
    REXLOG_ERROR(
        "Renut: Plume   frame {}{} vs={:016X} ps={:016X} x{} indices={} kind={} first={} pipeline={:X} fb={:X} "
        "s0={}B@{:X} s10={}B@{:X}",
        d.frame, d.pair_first_frame + 3 >= now ? " NEW" : "", d.vs, d.ps, group.draws, group.indices, d.kind,
        d.first, d.pipeline, d.framebuffer, d.s0_size, d.s0_addr, d.s10_size, d.s10_addr);
  }
}

void PlumeCommandProcessor::SyncEvery() {
  if (!list_open_) return;
  L()->end();
  list_open_ = false;
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  VkFence vk_fence = static_cast<VulkanCommandFence*>(fence_.get())->vk;
  const VkResult result = vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, 3000000000ull);
  if (result == VK_SUCCESS) vkResetFences(vk_device, 1, &vk_fence);
  StartList();
}

void PlumeCommandProcessor::SyncAfterDraw(const DrawPacket& pk) {
  if (!list_open_) return;
  L()->end();
  list_open_ = false;
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  VkFence vk_fence = static_cast<VulkanCommandFence*>(fence_.get())->vk;
  const auto wait_start = std::chrono::steady_clock::now();
  const VkResult result = vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, 3000000000ull);
  const auto wait_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count();
  ++sync_draw_count_;
  sync_recent_[sync_draw_count_ % sync_recent_.size()] = fmt::format("#{}: {}", sync_draw_count_, sync_draw_info_);
  if (result != VK_SUCCESS) {
    {
      unsigned long long avail_kb = 0;
      if (FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, meminfo)) {
          if (std::sscanf(line, "MemAvailable: %llu kB", &avail_kb) == 1) break;
        }
        std::fclose(meminfo);
      }
      REXLOG_ERROR("Renut: Plume system memory at the failure: {} MB available", avail_kb >> 10);
    }
    for (size_t i = 1; i <= sync_recent_.size(); ++i) {
      const std::string& line = sync_recent_[(sync_draw_count_ + i) % sync_recent_.size()];
      if (!line.empty()) REXLOG_ERROR("Renut: Plume   recent draw {}", line);
    }
    REXLOG_ERROR(
        "Renut: Plume GPU failed ({}) after {} ms on synced draw #{} in frame {}: vs={:016X} ps={:016X} kind={} "
        "count={} first={} base={} pipeline={}",
        int(result), wait_ms, sync_draw_count_, swaps_, pk.vs_hash, pk.ps_hash, int(pk.kind), pk.count, pk.first,
        pk.base_vertex, static_cast<const void*>(pk.pipeline));
    REXLOG_ERROR("Renut: Plume   {} streams of synced draws had a stale kept window before this one", sync_stale_streams_);
    const auto first_ok = sync_first_ok_.find(pk.vs_hash * 1099511628211ull ^ pk.ps_hash);
    if (first_ok != sync_first_ok_.end()) REXLOG_ERROR("Renut: Plume   first OK draw of this pair {}", first_ok->second);
    sync_draws_ = false;
  } else {
    vkResetFences(vk_device, 1, &vk_fence);
    if (sync_draw_info_.find("STALE") != std::string::npos && sync_stale_logged_ < 200) {
      ++sync_stale_logged_;
      REXLOG_INFO("Renut: Plume stale window under draw #{} frame {}: {}", sync_draw_count_, swaps_, sync_draw_info_);
    }
    if (sync_watch_ && (pk.vs_hash == sync_watch_ || pk.ps_hash == sync_watch_) && sync_watch_count_ < 600) {
      ++sync_watch_count_;
      REXLOG_INFO("Renut: Plume watched draw #{} frame {} pipeline={} fb={} ok: {}", sync_draw_count_, swaps_,
                  static_cast<const void*>(pk.pipeline), static_cast<const void*>(pk.framebuffer), sync_draw_info_);
    }
    // Each pair is named once, the first time it ran to completion.
    if (sync_seen_.insert(pk.vs_hash * 1099511628211ull ^ pk.ps_hash).second) {
      sync_first_ok_[pk.vs_hash * 1099511628211ull ^ pk.ps_hash] =
          fmt::format("frame {} pipeline={} fb={}: {}", swaps_, static_cast<const void*>(pk.pipeline),
                      static_cast<const void*>(pk.framebuffer), sync_draw_info_);
      REXLOG_INFO("Renut: Plume synced draw #{} ok, new pair (frame {}): vs={:016X} ps={:016X} count={}",
                  sync_draw_count_, swaps_, pk.vs_hash, pk.ps_hash, pk.count);
    }
  }
  StartList();
}

// Records the draws that are waiting, grouped by pipeline. The groups keep the
// order in which each pipeline first appeared and the draws keep their order
// inside a group, so a window whose draws share one pipeline is untouched.
void PlumeCommandProcessor::FlushDeferred(uint32_t reason) {
  if (deferred_.empty()) return;
  ++stats_.flush_windows[reason & 7];
  stats_.flush_draws[reason & 7] += deferred_.size();
  // Taken out first: recording goes through L(), which must find nothing waiting.
  flushing_.swap(deferred_);
  flushing_slots_.swap(deferred_slots_);
  deferred_.clear();
  deferred_slots_.clear();
  const size_t n = flushing_.size();
  defer_order_.resize(n);
  defer_groups_.clear();
  defer_group_start_.clear();
  uint32_t before = 0;
  for (size_t i = 0; i < n; ++i) {
    const DrawPacket& pk = flushing_[i];
    if (i == 0 || pk.pipeline != flushing_[i - 1].pipeline) ++before;
    auto [it, fresh] = defer_groups_.try_emplace(pk.pipeline, uint32_t(defer_group_start_.size()));
    if (fresh) defer_group_start_.push_back(0);
    ++defer_group_start_[it->second];
  }
  // Counts to start positions, then each draw to its place.
  uint32_t at = 0;
  for (uint32_t& entry : defer_group_start_) {
    const uint32_t count = entry;
    entry = at;
    at += count;
  }
  for (size_t i = 0; i < n; ++i) {
    defer_order_[defer_group_start_[defer_groups_[flushing_[i].pipeline]]++] = uint32_t(i);
  }
  uint32_t after = 0;
  for (size_t i = 0; i < n; ++i) {
    if (i == 0 || flushing_[defer_order_[i]].pipeline != flushing_[defer_order_[i - 1]].pipeline) ++after;
  }
  ++stats_.defer_windows;
  stats_.defer_binds_before += before;
  stats_.defer_binds_after += after;
  if (!HizFlushWindow(n)) {
    const bool async = RecActive();
    for (size_t i = 0; i < n; ++i) {
      const DrawPacket& pk = flushing_[defer_order_[i]];
      if (async) RecQueue(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
      else EmitDraw(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
    }
  }
  flushing_.clear();
  flushing_slots_.clear();
}


// The SDK works a resolve out from the registers and from the three vertices of
// the rectangle the guest's copy draw would have drawn (fetch constant 0), which
// it applies the window offset and the scissor to. Here the rectangle is known
// (x0/y0 inclusive, x1/y1 exclusive, from the Direct3D call itself), so the SDK
// is handed a register file that says exactly that: the same registers, with
// fetch constant 0 pointing at three vertices of the rectangle, and no window
// offset or scissor to apply.
// ---------------------------------------------------------------------------
// Hi-Z: boxes, frustum and scissor culling (see hiz_model.h)
// ---------------------------------------------------------------------------

void PlumeCommandProcessor::HizIndexBox(uint64_t key, const HizBox& box) {
  auto add = [&](uint32_t base, uint32_t size) {
    if (!size) return;
    for (uint32_t block = base >> 16; block <= (base + size - 1) >> 16; ++block) {
      hiz_box_blocks_.emplace(block, key);
    }
  };
  add(box.vertex_base, box.vertex_size);
  add(box.index_base, box.index_size);
  // Entries of dropped boxes pile up; rebuilt from the live ones now and then.
  if (hiz_box_blocks_.size() > 400000) {
    hiz_box_blocks_.clear();
    for (const auto& [k, b] : hiz_boxes_) {
      if (b.vertex_size) {
        for (uint32_t block = b.vertex_base >> 16; block <= (b.vertex_base + b.vertex_size - 1) >> 16; ++block) {
          hiz_box_blocks_.emplace(block, k);
        }
      }
      if (b.index_size) {
        for (uint32_t block = b.index_base >> 16; block <= (b.index_base + b.index_size - 1) >> 16; ++block) {
          hiz_box_blocks_.emplace(block, k);
        }
      }
    }
  }
}

// The guest wrote [start, start + length): the boxes made from any of it go.
void PlumeCommandProcessor::HizDropBoxes(uint32_t start, uint32_t length) {
  if (!length) return;
  ++hiz_box_generation_;
  const uint32_t first = start >> 16, last = uint32_t((uint64_t(start) + length - 1) >> 16);
  if (last - first > 4096) {
    // A huge range: cheaper to look at every box.
    for (auto it = hiz_boxes_.begin(); it != hiz_boxes_.end();) {
      const HizBox& box = it->second;
      const bool hit = (box.vertex_size && box.vertex_base < uint64_t(start) + length &&
                        start < box.vertex_base + box.vertex_size) ||
                       (box.index_size && box.index_base < uint64_t(start) + length &&
                        start < box.index_base + box.index_size);
      it = hit ? hiz_boxes_.erase(it) : std::next(it);
    }
    return;
  }
  for (uint32_t block = first; block <= last; ++block) {
    auto range = hiz_box_blocks_.equal_range(block);
    for (auto it = range.first; it != range.second;) {
      hiz_boxes_.erase(it->second);
      it = hiz_box_blocks_.erase(it);
    }
  }
}

PlumeCommandProcessor::HizModelEntry* PlumeCommandProcessor::HizModelFor(HostShader& vs) {
  if (!vs.bool_mask_decoded) {
    rex::string::StringBuffer disassembly;
    vs.AnalyzeUcode(disassembly);
    std::memcpy(vs.bool_mask, vs.constant_register_map().bool_bitmap, sizeof(vs.bool_mask));
    vs.bool_mask_decoded = true;
  }
  const gfx::RegisterFile& regs = *register_file_;
  uint32_t bools[8];
  bool same = vs.hiz_entry != nullptr;
  for (uint32_t word = 0; word < 8; ++word) {
    bools[word] = regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + word] & vs.bool_mask[word];
    same = same && bools[word] == vs.hiz_bools[word];
  }
  if (same) return static_cast<HizModelEntry*>(vs.hiz_entry);
  uint64_t key = vs.ucode_data_hash() * 0x9E3779B97F4A7C15ull;
  for (uint32_t word = 0; word < 8; ++word) key = (key ^ bools[word]) * 0x100000001B3ull;
  auto it = hiz_models_.find(key);
  if (it != hiz_models_.end()) {
    vs.hiz_entry = &it->second;
    std::memcpy(vs.hiz_bools, bools, sizeof(bools));
    return &it->second;
  }
  const auto start = std::chrono::steady_clock::now();
  HizModelEntry entry;
  entry.model = renut::hiz::BuildModel(vs.ucode_dwords(), vs.ucode_dword_count(), bools);
  ++stats_.hiz_models;
  if (entry.model->valid) ++stats_.hiz_models_valid;
  stats_.hiz_model_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start).count());
  HizModelEntry* made = &hiz_models_.emplace(key, std::move(entry)).first->second;
  vs.hiz_entry = made;
  std::memcpy(vs.hiz_bools, bools, sizeof(bools));
  return made;
}

// The model against the SDK's interpreter, on two vertices of the draw about to
// use it: what the symbolic executor claims must be what the microcode does.
bool PlumeCommandProcessor::HizValidate(HostShader& vs, renut::hiz::Model& model,
                                        const xenos::xe_gpu_vertex_fetch_t& fetch, uint32_t vertex_a,
                                        uint32_t vertex_b) {
  if (!gfx::ShaderInterpreter::CanInterpretShader(vs)) return false;
  struct Sink final : gfx::ShaderInterpreter::ExportSink {
    float position[4] = {};
    uint32_t written = 0;
    void Export(rex::graphics::ucode::ExportRegister reg, const float* value, uint32_t mask) override {
      if (reg != rex::graphics::ucode::ExportRegister::kVSPosition) return;
      for (uint32_t i = 0; i < 4; ++i) {
        if (mask & (1u << i)) {
          position[i] = value[i];
          written |= 1u << i;
        }
      }
    }
  } sink;
  auto interpreter = std::make_unique<gfx::ShaderInterpreter>(*register_file_, *memory_);
  interpreter->SetShader(vs);
  interpreter->SetExportSink(&sink);
  const float* constants = reinterpret_cast<const float*>(&register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  float affine[4][5];
  renut::hiz::Evaluate(model, constants, 0, affine);
  const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(0);
  for (uint32_t vertex : {vertex_a, vertex_b}) {
    sink.written = 0;
    interpreter->temp_registers()[0] = float(vertex);
    interpreter->Execute();
    float p[4] = {};
    if (!renut::hiz::FetchVertex(model.fetch, memory, fetch.address, fetch.size, uint32_t(fetch.endian), vertex, p)) {
      return false;
    }
    if (sink.written != 0xF) return false;
    for (int o = 0; o < 4; ++o) {
      float predicted = affine[o][0];
      for (int j = 0; j < 4; ++j) predicted += affine[o][1 + j] * p[j];
      const float reference = sink.position[o];
      if (!(std::fabs(predicted - reference) <= 2e-3f * (1.0f + std::fabs(predicted) + std::fabs(reference)))) {
        static uint32_t logged = 0;
        if (logged < 12) {
          ++logged;
          REXLOG_WARN("Renut: Plume Hi-Z model of vs {:016X} rejected: vertex {} clip[{}] model {} interpreter {}",
                      vs.ucode_data_hash(), vertex, o, predicted, reference);
        }
        return false;
      }
    }
  }
  return true;
}

const PlumeCommandProcessor::HizBox* PlumeCommandProcessor::HizBoxFor(
    const renut::hiz::Model& model, const xenos::xe_gpu_vertex_fetch_t& fetch, const IndexBufferInfo* index_info,
    uint32_t index_count, int32_t base_vertex) {
  DrainWatchHits();
  const bool indexed = index_info && index_info->guest_base;
  const renut::hiz::FetchDesc& f = model.fetch;
  uint64_t parts[16];
  uint32_t n = 0;
  parts[n++] = (uint64_t(fetch.address) << 32) | fetch.size;
  parts[n++] = (uint64_t(uint32_t(fetch.endian)) << 56) | (uint64_t(f.fetch_constant) << 48) |
               (uint64_t(f.format) << 40) | (uint64_t(f.is_signed) << 32) | (uint64_t(f.normalized) << 33) |
               (uint64_t(f.signed_no_zero) << 34) | (uint64_t(f.used_mask) << 36) |
               uint64_t(uint32_t(f.exp_adjust) & 0x3F);
  parts[n++] = (uint64_t(f.stride_dwords) << 32) | uint32_t(f.offset_dwords);
  parts[n++] = indexed ? ((uint64_t(index_info->guest_base) << 32) | index_count) : index_count;
  parts[n++] = indexed ? ((uint64_t(uint32_t(index_info->format)) << 8) | uint64_t(uint32_t(index_info->endianness)))
                       : 0xFFFF;
  parts[n++] = uint64_t(uint32_t(base_vertex));
  // A multiply-and-rotate mix of the six words: the key only has to tell two
  // meshes apart, and this ran for every draw.
  uint64_t key = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < n; ++i) {
    key = (key ^ parts[i]) * 0xFF51AFD7ED558CCDull;
    key ^= key >> 32;
  }
  hiz_last_box_key_ = key;
  HizFront& front = hiz_front_[key & (hiz_front_.size() - 1)];
  if (front.key == key && front.generation == hiz_box_generation_ && front.box &&
      swaps_ - front.box->checked_frame < 30) {
    ++stats_.hiz_box_hits;
    return front.box->valid ? front.box : nullptr;
  }

  const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(0);
  auto checksum_of = [&](const HizBox& box) {
    uint64_t h = 0;
    auto add = [&](uint32_t base, uint32_t size) {
      if (!size) return;
      const uint32_t head = std::min<uint32_t>(size, 128);
      h = XXH3_64bits_withSeed(memory + base, head, h);
      if (size > head) h = XXH3_64bits_withSeed(memory + base + size - head, head, h);
    };
    add(box.vertex_base, box.vertex_size);
    add(box.index_base, box.index_size);
    return h;
  };
  auto found = hiz_boxes_.find(key);
  if (found != hiz_boxes_.end()) {
    HizBox& box = found->second;
    // The write watch drops a box when the guest writes under it; the sample of
    // its bytes is a second line, looked at every 30 frames rather than at every draw.
    if (swaps_ - box.checked_frame < 30) {
      ++stats_.hiz_box_hits;
      front = {key, &box, hiz_box_generation_};
      return box.valid ? &box : nullptr;
    }
    if (checksum_of(box) == box.checksum) {
      box.checked_frame = swaps_;
      ++stats_.hiz_box_hits;
      front = {key, &box, hiz_box_generation_};
      return box.valid ? &box : nullptr;
    }
    hiz_boxes_.erase(found);
    ++hiz_box_generation_;
  }

  const auto start = std::chrono::steady_clock::now();
  HizBox box;
  box.vertex_base = fetch.address << 2;
  box.vertex_size = fetch.size << 2;
  const bool is32 = indexed && index_info->format == xenos::IndexFormat::kInt32;
  if (indexed) {
    box.index_base = index_info->guest_base;
    box.index_size = index_count * (is32 ? 4 : 2);
  }
  // `cache` false hands the box back through hiz_scratch_box_ instead of the map.
  auto finish = [&](bool valid, bool cache) -> const HizBox* {
    stats_.hiz_box_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start).count());
    ++stats_.hiz_boxes_made;
    box.valid = valid;
    if (!valid && !cache) return nullptr;
    if (!cache) {
      hiz_scratch_box_ = box;
      return &hiz_scratch_box_;
    }
    box.checksum = checksum_of(box);
    box.checked_frame = swaps_;
    const HizBox* stored = &hiz_boxes_.emplace(key, box).first->second;
    HizIndexBox(key, box);
    front = {key, stored, hiz_box_generation_};
    return valid ? stored : nullptr;
  };
  if (!box.vertex_size || uint64_t(box.vertex_base) + box.vertex_size > 0x20000000ull ||
      (indexed && (uint64_t(box.index_base) + box.index_size > 0x20000000ull || !index_count))) {
    return finish(false, false);
  }
  // Index range.
  int64_t lowest = INT64_MAX, highest = -1;
  if (indexed) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(memory + box.index_base);
    const uint16_t* halves = reinterpret_cast<const uint16_t*>(memory + box.index_base);
    xenos::Endian endian = index_info->endianness;
    if (!is32) {
      if (endian == xenos::Endian::k8in32) endian = xenos::Endian::k8in16;
      else if (endian == xenos::Endian::k16in32) endian = xenos::Endian::kNone;
    }
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t v = is32 ? words[i] : halves[i];
      v = xenos::GpuSwap(v, endian);
      if (!is32) v &= 0xFFFF;
      if (v == (is32 ? 0xFFFFFFFFu : 0xFFFFu)) continue;  // primitive restart
      v &= 0xFFFFFF;
      lowest = std::min<int64_t>(lowest, v);
      highest = std::max<int64_t>(highest, v);
    }
    if (highest < 0) return finish(false, true);
  } else {
    lowest = 0;
    highest = int64_t(index_count) - 1;
  }
  const int64_t first = lowest + base_vertex, last = highest + base_vertex;
  if (first < 0 || last - first > 20000) return finish(false, true);
  float lo[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX}, hi[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (int64_t v = first; v <= last; ++v) {
    float p[4];
    if (!renut::hiz::FetchVertex(f, memory, fetch.address, fetch.size, uint32_t(fetch.endian), uint32_t(v), p)) {
      return finish(false, true);
    }
    for (int j = 0; j < 4; ++j) {
      if (!std::isfinite(p[j])) return finish(false, true);
      lo[j] = std::min(lo[j], p[j]);
      hi[j] = std::max(hi[j], p[j]);
    }
  }
  std::memcpy(box.lo, lo, sizeof(lo));
  std::memcpy(box.hi, hi, sizeof(hi));
  box.first_vertex = uint32_t(first);
  box.last_vertex = uint32_t(last);
  // What the box was made from must be watched, or a guest rewrite of the mesh
  // (streaming, a dynamic buffer) would leave it stale: the checksum above only
  // samples the ends.
  const auto access = [&](uint32_t base, uint32_t size) {
    return !size || memory_->GetPhysicalHeap()->QueryRangeAccess(base, base + size - 1) ==
                       rex::memory::PageAccess::kReadWrite;
  };
  if (!access(box.vertex_base, box.vertex_size) || !access(box.index_base, box.index_size)) {
    // Made again for the next draw; the range is bounded above (20000 vertices).
    return finish(true, false);
  }
  memory_->EnablePhysicalMemoryAccessCallbacks(box.vertex_base, box.vertex_size, true, false);
  if (box.index_size) memory_->EnablePhysicalMemoryAccessCallbacks(box.index_base, box.index_size, true, false);
  return finish(true, true);
}

bool PlumeCommandProcessor::HizCullDraw(HostShader& vs, xenos::PrimitiveType prim_type, uint32_t index_count,
                                        const IndexBufferInfo* index_info, bool color_pass) {
  const uint32_t kind = color_pass ? 1 : 0;
  hiz_test_.valid = false;
  ++stats_.hiz_seen[kind];
  if (!index_count) return ++stats_.hiz_why[0], false;
  switch (prim_type) {
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kQuadList: break;
    default: return ++stats_.hiz_why[1], false;
  }
  const gfx::RegisterFile& regs = *register_file_;
  if (vs.entry && (vs.entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) return ++stats_.hiz_why[2], false;
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  // vtx_w0_fmt set is the usual case: the shader's W is W, not 1/W.
  if (clip.clip_disable || vte.vtx_xy_fmt || vte.vtx_z_fmt || !vte.vtx_w0_fmt || !vte.vport_x_scale_ena ||
      !vte.vport_x_offset_ena || !vte.vport_y_scale_ena || !vte.vport_y_offset_ena) {
    ++hiz_clip_states_[(uint64_t(vte.value) << 32) | clip.value];
    return ++stats_.hiz_why[3], false;
  }
  if (regs.Get<reg::SQ_VS_CONST>(XE_GPU_REG_SQ_VS_CONST).base != 0) return ++stats_.hiz_why[4], false;

  const auto stage_a = hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  HizModelEntry* entry = HizModelFor(vs);
  if (hiz_stage_timing_) {
    stats_.hiz_stage_ns[0] += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - stage_a).count());
  }
  if (!entry->model || !entry->model->valid) {
    ++stats_.hiz_no_model[kind];
    ++stats_.hiz_why_model[kind][entry->model ? uint32_t(entry->model->why) % 12 : 0];
    ++hiz_nomodel_draws_[(vs.entry ? vs.entry->hash : 0) ^ (entry->model ? uint64_t(entry->model->why) & 15 : 0)];
    hiz_nomodel_why_[vs.entry ? vs.entry->hash : 0] = entry->model ? uint32_t(entry->model->why) : 0;
    return false;
  }
  renut::hiz::Model& model = *entry->model;
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(model.fetch.fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) {
    ++stats_.hiz_no_box[kind];
    return false;
  }
  const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
  // A mesh whose draws are almost never found outside (under 1 in 16, over the
  // last 64-128 draws of it, every instance together) costs more in tests than
  // the few culls save: break-even is about one cull in ten. Only every 8th draw
  // of it is tested; a cull found there counts for 8, so the estimate stays
  // unbiased and a mesh that starts to leave the view is tested again within a
  // few draws. Skipping a test is always safe: the draw is just made. The mesh
  // is named by a cheap key (model, fetch range, index range), so a skipped draw
  // never reaches the box lookup either.
  HizCold* cold_slot = nullptr;
  bool sampled_cold = false;
  if (hiz_skip_ && hiz_mode_ == 2) {
    const uint64_t index_word = index_info && index_info->guest_base
                                    ? (uint64_t(index_info->guest_base) << 32) | index_count
                                    : uint64_t(index_count);
    uint64_t k = (uint64_t(reinterpret_cast<uintptr_t>(entry)) ^ 0x9E3779B97F4A7C15ull) * 0xFF51AFD7ED558CCDull;
    k = (k ^ ((uint64_t(fetch.address) << 32) | fetch.size)) * 0xC4CEB9FE1A85EC53ull;
    k = (k ^ index_word ^ (uint64_t(uint32_t(base_vertex)) << 40)) * 0xFF51AFD7ED558CCDull;
    k ^= k >> 29;
    cold_slot = &hiz_cold_[k & (hiz_cold_.size() - 1)];
    if (cold_slot->key != k) *cold_slot = HizCold{k, 0, 0, 0};
    if (cold_slot->seen >= 128) {
      cold_slot->seen >>= 1;
      cold_slot->culled >>= 1;
    }
    const bool cold = cold_slot->seen >= 32 && cold_slot->culled * 16 < cold_slot->seen;
    ++cold_slot->seen;
    if (cold) {
      if ((++cold_slot->phase & 7) != 0) {
        ++stats_.hiz_skipped[kind];
        return false;
      }
      sampled_cold = true;
    }
  }
  const auto stage_b = hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  const HizBox* box = HizBoxFor(model, fetch, index_info, index_count, base_vertex);
  if (hiz_stage_timing_) {
    stats_.hiz_stage_ns[1] += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - stage_b).count());
  }
  if (!box) {
    ++stats_.hiz_no_box[kind];
    return false;
  }
  if (!entry->validated) {
    // First use: the first and the last vertex of the draw through the SDK's
    // interpreter against the model.
    if (!HizValidate(vs, model, fetch, box->first_vertex, box->last_vertex)) {
      model.valid = false;
      ++stats_.hiz_model_rejected;
      ++stats_.hiz_no_model[kind];
      return false;
    }
    entry->validated = true;
  }
  // The constants as the shader sees them, and the model's affine map.
  const float* constants = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  float affine[4][5];
  const auto stage_c = hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  renut::hiz::Evaluate(model, constants, 0, affine);
  if (hiz_stage_timing_) {
    stats_.hiz_stage_ns[2] += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - stage_c).count());
  }
  struct StageGuard {
    uint64_t* out;
    std::chrono::steady_clock::time_point begin;
    bool on;
    ~StageGuard() {
      if (on) {
        *out += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - begin).count());
      }
    }
  } plane_stage{&stats_.hiz_stage_ns[3],
                hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point(),
                hiz_stage_timing_};
  // The box in clip space is a parallelepiped: centre C and, along the element's
  // components, the columns A[.][1 + j] * half_j. A plane's value over it ranges
  // over centre +- sum |coefficient along j| * half_j, so "all of it outside" is
  // one comparison per plane and no corner is formed.
  float centre[4], half[4], mid[4];
  uint32_t comps[4], ncomps = 0;
  for (uint32_t j = 0; j < 4; ++j) {
    mid[j] = 0.5f * (box->lo[j] + box->hi[j]);
    half[j] = 0.5f * (box->hi[j] - box->lo[j]);
    if (!(model.fetch.used_mask & (1u << j))) continue;
    bool any = false;
    for (int o = 0; o < 4; ++o) any = any || affine[o][1 + j] != 0.f;
    if (any) comps[ncomps++] = j;
  }
  float magnitude = 0.f;
  for (int o = 0; o < 4; ++o) {
    float c = affine[o][0];
    for (uint32_t i = 0; i < ncomps; ++i) {
      const uint32_t j = comps[i];
      c += affine[o][1 + j] * mid[j];
      magnitude += std::fabs(affine[o][1 + j] * half[j]);
    }
    if (!std::isfinite(c)) return false;
    centre[o] = c;
    magnitude += std::fabs(c);
  }
  ++stats_.hiz_eligible[kind];
  // [lowest, highest] of the plane s . clip over the box.
  auto plane_range = [&](float sx, float sy, float sz, float sw, float* lowest, float* highest) {
    float c = sx * centre[0] + sy * centre[1] + sz * centre[2] + sw * centre[3];
    float r = 0.f;
    for (uint32_t i = 0; i < ncomps; ++i) {
      const uint32_t j = comps[i];
      r += std::fabs(sx * affine[0][1 + j] + sy * affine[1][1 + j] + sz * affine[2][1 + j] + sw * affine[3][1 + j]) *
           half[j];
    }
    *lowest = c - r;
    *highest = c + r;
  };
  const float slack = 1e-4f * magnitude;
  auto wholly_outside = [&](float sx, float sy, float sz, float sw) {
    float low, high;
    plane_range(sx, sy, sz, sw, &low, &high);
    return high < -slack;
  };
  if (wholly_outside(1, 0, 0, 1) || wholly_outside(-1, 0, 0, 1) || wholly_outside(0, 1, 0, 1) ||
      wholly_outside(0, -1, 0, 1) ||
      (clip.dx_clip_space_def && (wholly_outside(0, 0, 1, 0) || wholly_outside(0, 0, -1, 1)))) {
    ++stats_.hiz_frustum[kind];
    if (cold_slot) cold_slot->culled += sampled_cold ? 8 : 1;
    return hiz_mode_ >= 2;
  }
  // Wholly in front of the eye: the scissor and the screen rectangle make sense.
  float w_low, w_high;
  plane_range(0, 0, 0, 1, &w_low, &w_high);
  if (!(w_low > 1e-6f)) return false;
  float window_x = 0.f, window_y = 0.f;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    window_x = float(offset.window_x_offset);
    window_y = float(offset.window_y_offset);
  }
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    window_x += 0.5f;
    window_y += 0.5f;
  }
  const float xs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  const float xo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  const float ys = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  const float yo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const float left = float(scissor.offset[0]), top = float(scissor.offset[1]);
  const float right = left + float(scissor.extent[0]), bottom = top + float(scissor.extent[1]);
  // x_screen = xo + window_x + xs * x / w. All of the box left of (left - 1):
  // (left - 1 - xo - window_x) * w - xs * x > 0 everywhere, and so on.
  auto all_positive = [&](float sx, float sy, float sw) {
    float low, high;
    plane_range(sx, sy, 0, sw, &low, &high);
    return low > slack;
  };
  const float bx = xo + window_x, by = yo + window_y;
  if (all_positive(-xs, 0, left - 1.0f - bx) || all_positive(xs, 0, -(right + 1.0f - bx)) ||
      all_positive(0, -ys, top - 1.0f - by) || all_positive(0, ys, -(bottom + 1.0f - by))) {
    ++stats_.hiz_scissor[kind];
    if (cold_slot) cold_slot->culled += sampled_cold ? 8 : 1;
    return hiz_mode_ >= 2;
  }
  if (hiz_mode_ >= 3 && clip.dx_clip_space_def) {
    // The occlusion test wants the rectangle and the nearest depth: corners.
    const float zs = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.f;
    const float zo = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.f;
    const float depth_min = std::clamp(zo, 0.f, 1.f), depth_max = std::clamp(zo + zs, 0.f, 1.f);
    float min_x = FLT_MAX, max_x = -FLT_MAX, min_y = FLT_MAX, max_y = -FLT_MAX, nearest = FLT_MAX;
    const uint32_t corners = 1u << ncomps;
    for (uint32_t c = 0; c < corners; ++c) {
      float clip_v[4];
      for (int o = 0; o < 4; ++o) {
        float v = affine[o][0];
        for (uint32_t i = 0; i < ncomps; ++i) {
          const uint32_t j = comps[i];
          v += affine[o][1 + j] * (((c >> i) & 1) ? box->hi[j] : box->lo[j]);
        }
        clip_v[o] = v;
      }
      const float inv = 1.0f / clip_v[3];
      const float x = bx + xs * clip_v[0] * inv;
      const float y = by + ys * clip_v[1] * inv;
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
      const float z = std::clamp(clip_v[2] * inv, 0.f, 1.f);
      nearest = std::min(nearest, depth_min + z * (depth_max - depth_min));
    }
    hiz_test_.valid = true;
    // The rectangle is also cut to the scissor: nothing outside it is drawn.
    hiz_test_.rect[0] = std::max(min_x - 1.0f, left);
    hiz_test_.rect[1] = std::max(min_y - 1.0f, top);
    hiz_test_.rect[2] = std::min(max_x + 1.0f, right);
    hiz_test_.rect[3] = std::min(max_y + 1.0f, bottom);
    hiz_test_.z = nearest;
    hiz_test_.key = hiz_last_box_key_ ^ (vs.ucode_data_hash() * 0x9E3779B97F4A7C15ull);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Hi-Z: occlusion by the depth the pass has drawn so far
//
// A window of deferred draws may be recorded in any order (see IssueDraw). The
// draws a window holds that were wholly hidden the last time they were tested
// are kept back; the rest are recorded as usual. Then, with the pass closed, the
// depth image is copied out, reduced to a pyramid of its farthest values, and
// every boxed draw of the window is tested against it on the GPU (a rectangle
// and the box's nearest depth). The kept-back draws are recorded last, each
// inside conditional rendering on its own answer: a draw that is still hidden
// costs the GPU next to nothing, one that is not hidden runs - in the same
// frame, so nothing arrives late and nothing can be wrong by a frame. What the
// tests found feeds the prediction for the frames after.
// ---------------------------------------------------------------------------

bool PlumeCommandProcessor::HizCreatePipelines() {
  if (hiz_pipelines_[2] != VK_NULL_HANDLE) return true;
  if (hiz_pipelines_failed_) return false;
  hiz_pipelines_failed_ = true;
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  if (hiz_layout_ == VK_NULL_HANDLE) {
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &hiz_layout_) != VK_SUCCESS) return false;
  }
  const std::pair<const uint32_t*, size_t> code[3] = {{k_hiz_reduce_comp, sizeof(k_hiz_reduce_comp)},
                                                      {k_hiz_mips_comp, sizeof(k_hiz_mips_comp)},
                                                      {k_hiz_test_comp, sizeof(k_hiz_test_comp)}};
  for (uint32_t i = 0; i < 3; ++i) {
    if (hiz_pipelines_[i] != VK_NULL_HANDLE) continue;
    VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = code[i].second;
    module_info.pCode = code[i].first;
    if (vkCreateShaderModule(vk_device, &module_info, nullptr, &hiz_modules_[i]) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = hiz_modules_[i];
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = hiz_layout_;
    if (vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &hiz_pipelines_[i]) !=
        VK_SUCCESS) {
      hiz_pipelines_[i] = VK_NULL_HANDLE;
      REXLOG_WARN("Renut: Plume Hi-Z compute pipelines unavailable; occlusion culling stays off");
      return false;
    }
  }
  hiz_pipelines_failed_ = false;
  return true;
}

void PlumeCommandProcessor::HizDestroy() {
  if (!device_) return;
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  for (auto& pipeline : hiz_pipelines_) {
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  for (auto& module : hiz_modules_) {
    if (module != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, module, nullptr);
    module = VK_NULL_HANDLE;
  }
  if (hiz_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, hiz_layout_, nullptr);
  hiz_layout_ = VK_NULL_HANDLE;
  if (hiz_query_pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(vk_device, hiz_query_pool_, nullptr);
  hiz_query_pool_ = VK_NULL_HANDLE;
  hiz_pyramid_.reset();
  for (auto& buffer : hiz_pred_) buffer.reset();
  for (auto& buffer : hiz_read_) buffer.reset();
}

// What a window's test needs, made once: the pipelines, the depth copy's
// buffer (shared with the depth resolves), the pyramid, and this slot's
// predicate and readback buffers.
bool PlumeCommandProcessor::HizEnsureResources() {
  if (!hiz_cond_ || !HizCreatePipelines()) return false;
  constexpr uint64_t kStagingSize = 4096ull * 2048 * 4;
  if (!depth_staging_) {
    depth_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        kStagingSize, RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!depth_staging_) return false;
  }
  if (!hiz_pyramid_) {
    // 4096x2048 reduced by 8, then halved four times: 1.34 x 512 x 256 floats.
    hiz_pyramid_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        uint64_t(1) << 20, RenderBufferFlag::STORAGE | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!hiz_pyramid_) return false;
  }
  if (!hiz_pred_[slot_]) {
    hiz_pred_[slot_] = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        uint64_t(kHizMaxRecords) * 8, RenderBufferFlag::STORAGE | RenderBufferFlag::DEVICE_ADDRESSABLE));
    // The answers, then the farthest depth each test found, then a debug copy.
    hiz_read_[slot_] = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(kHizMaxRecords) * 8 + 65536 * 4));
    if (!hiz_pred_[slot_] || !hiz_read_[slot_]) {
      hiz_pred_[slot_].reset();
      hiz_read_[slot_].reset();
      return false;
    }
  }
  return true;
}

// Tests the records [first, first + count) of hiz_records_ against `target`'s
// depth. On success the answers are in hiz_pred_[slot_] from record `base`
// (hiz_pred_used_) on, and the render pass is closed (the next draw reopens it).
bool PlumeCommandProcessor::HizRunTest(Target& target, uint32_t count, uint32_t* base_out) {
  const uint32_t w = target.width, h = target.height;
  if (!count || uint64_t(w) * h * 4 > 4096ull * 2048 * 4 || target.format != RenderFormat::D32_FLOAT_S8_UINT) {
    return false;
  }
  if (hiz_pred_used_[slot_] + count > kHizMaxRecords) return false;
  // Levels: 8 px blocks, then 16, 32, 64, 128.
  constexpr uint32_t kLevels = 5;
  uint32_t lw[kLevels], lh[kLevels], offset[kLevels];
  uint32_t total = 0;
  lw[0] = (w + 7) / 8;
  lh[0] = (h + 7) / 8;
  for (uint32_t l = 1; l < kLevels; ++l) {
    lw[l] = (lw[l - 1] + 1) / 2;
    lh[l] = (lh[l - 1] + 1) / 2;
  }
  for (uint32_t l = 0; l < kLevels; ++l) {
    offset[l] = total;
    total += lw[l] * lh[l];
  }
  if (uint64_t(total) * 4 > (uint64_t(1) << 20)) return false;
  const uint64_t records_offset = Allocate(uint64_t(count) * 32, 256);
  if (records_offset == UINT64_MAX) return false;
  auto* out = reinterpret_cast<float*>(Pointer(records_offset));
  for (uint32_t i = 0; i < count; ++i) {
    const HizRecord& r = hiz_records_[i];
    float* record = out + size_t(i) * 8;
    record[0] = r.rect[0];
    record[1] = r.rect[1];
    record[2] = r.rect[2];
    record[3] = r.rect[3];
    record[4] = r.z;
    record[5] = record[6] = record[7] = 0.f;
  }
  const uint32_t base = hiz_pred_used_[slot_];
  // Closes the pass and puts the depth where it can be copied from.
  L()->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.texture.get(), RenderTextureLayout::COPY_SOURCE));
  ++stats_.barriers;
  target.layout = RenderTextureLayout::COPY_SOURCE;
  bound_framebuffer_ = nullptr;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(L())->vk;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cb, static_cast<VulkanTexture*>(target.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(depth_staging_.get())->vk, 1,
                         &region);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  const uint64_t depth_address = depth_staging_->getDeviceAddress();
  const uint64_t pyramid_address = hiz_pyramid_->getDeviceAddress();
  auto compute_barrier = [&]() {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  };
  // Level 0 from the depth, then each from the one before.
  {
    struct {
      uint64_t src, dst;
      uint32_t width, height, bw, bh;
    } push = {depth_address, pyramid_address + uint64_t(offset[0]) * 4, w, h, lw[0], lh[0]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipelines_[0]);
    vkCmdPushConstants(cb, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (lw[0] * lh[0] + 63) / 64, 1, 1);
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipelines_[1]);
  for (uint32_t l = 1; l < kLevels; ++l) {
    compute_barrier();
    struct {
      uint64_t src, dst;
      uint32_t sw, sh, dw, dh;
    } push = {pyramid_address + uint64_t(offset[l - 1]) * 4, pyramid_address + uint64_t(offset[l]) * 4,
              lw[l - 1], lh[l - 1], lw[l], lh[l]};
    vkCmdPushConstants(cb, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (lw[l] * lh[l] + 63) / 64, 1, 1);
  }
  compute_barrier();
  {
    struct {
      uint64_t records, predicate, pyramid;
      uint32_t count, levels;
      uint32_t offsets[6], widths[6], heights[6];
    } push = {};
    push.records = Address(records_offset);
    push.predicate = hiz_pred_[slot_]->getDeviceAddress() + uint64_t(base) * 4;
    push.pyramid = pyramid_address;
    push.count = count;
    push.levels = kLevels;
    for (uint32_t l = 0; l < kLevels; ++l) {
      push.offsets[l] = offset[l];
      push.widths[l] = lw[l];
      push.heights[l] = lh[l];
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipelines_[2]);
    vkCmdPushConstants(cb, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
  }
  // The answers go to conditional rendering, and to the readback buffer.
  {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  }
  VkBufferCopy copies[2] = {{uint64_t(base) * 4, uint64_t(base) * 4, uint64_t(count) * 4},
                            {uint64_t(kHizMaxRecords + base) * 4, uint64_t(kHizMaxRecords + base) * 4,
                             uint64_t(count) * 4}};
  vkCmdCopyBuffer(cb, static_cast<VulkanBuffer*>(hiz_pred_[slot_].get())->vk,
                  static_cast<VulkanBuffer*>(hiz_read_[slot_].get())->vk, hiz_verify_ ? 2 : 1, copies);
  // Debug: the first window of some frames also copies its level 0 out.
  if (hiz_debug_ && !hiz_debug_pending_[slot_] && swaps_ % 60 == 0 && lw[0] * lh[0] <= 65536) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
    VkBufferCopy debug_copy = {uint64_t(offset[0]) * 4, uint64_t(kHizMaxRecords) * 8, uint64_t(lw[0]) * lh[0] * 4};
    vkCmdCopyBuffer(cb, static_cast<VulkanBuffer*>(hiz_pyramid_.get())->vk,
                    static_cast<VulkanBuffer*>(hiz_read_[slot_].get())->vk, 1, &debug_copy);
    hiz_debug_pending_[slot_] = lw[0] * lh[0];
    hiz_debug_rects_[slot_].clear();
    for (uint32_t i = 0; i < count && i < 64; ++i) {
      hiz_debug_rects_[slot_].push_back({hiz_records_[i].rect[0], hiz_records_[i].rect[1], hiz_records_[i].rect[2],
                                         hiz_records_[i].rect[3], hiz_records_[i].z});
    }
  }
  // The compute layout's push constants may disturb the graphics ones.
  pushed_constants_[0] = pushed_constants_[1] = pushed_constants_[2] = UINT64_MAX;
  hiz_pred_used_[slot_] += count;
  *base_out = base;
  ++stats_.hiz_windows_tested;
  stats_.hiz_records += count;
  // Back to a depth buffer the next draw can write.
  Transition(target.texture.get(), target.layout, RenderTextureLayout::DEPTH_WRITE);
  return true;
}

// Records a window with the Hi-Z test. False leaves it to the plain loop.
//
// A test costs a pass break, a copy of the depth and a few dispatches, so it is
// only paid for where it can win: a window holding draws that were hidden
// before (their answers are needed, in this frame, to run them or not), and now
// and then a few windows that hold nothing back, to find out which draws are
// hidden. When what the tests find drops under a few percent the second kind
// becomes rare.
bool PlumeCommandProcessor::HizFlushWindow(size_t n) {
  if (hiz_mode_ < 3 || !hiz_cond_) return false;
  Target* depth = nullptr;
  uint32_t testable = 0, remembered = 0;
  for (size_t i = 0; i < n; ++i) {
    const DrawPacket& pk = flushing_[i];
    if (!pk.hiz_depth || !pk.hiz_depth->texture || pk.hiz_depth->texture.get() != pk.hiz_texture) continue;
    if (!depth) depth = pk.hiz_depth;
    if (pk.hiz_depth != depth) continue;
    ++testable;
    auto found = hiz_history_.find(pk.hiz_key);
    if (hiz_force_ || (found != hiz_history_.end() && found->second.hidden && swaps_ - found->second.frame <= 90)) {
      ++remembered;
    }
  }
  ++stats_.hiz_windows_all;
  if (!depth || !testable) return false;
  const bool learning = testable >= 3 && hiz_learn_left_ > 0 && swaps_ % hiz_learn_period_ < 2;
  if (!remembered && !learning) {
    ++stats_.hiz_small_windows;
    stats_.hiz_small_draws += testable;
    return false;
  }
  if (hiz_tests_frame_ >= (remembered ? 12u : 3u) + (remembered ? 0u : hiz_tests_remembered_) ||
      !HizEnsureResources()) {
    return false;
  }
  ++hiz_tests_frame_;
  if (remembered) ++hiz_tests_remembered_;
  else --hiz_learn_left_;
  auto usable = [&](const DrawPacket& pk) {
    return pk.hiz_depth == depth && pk.hiz_texture == depth->texture.get();
  };
  // Recorded now, in the grouped order: whatever is not known to be hidden.
  hiz_records_.clear();
  hiz_held_.clear();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t index = defer_order_[i];
    const DrawPacket& pk = flushing_[index];
    if (usable(pk)) {
      auto found = hiz_history_.find(pk.hiz_key);
      if (hiz_force_ || (found != hiz_history_.end() && found->second.hidden && swaps_ - found->second.frame <= 90)) {
        hiz_held_.push_back(index);
        continue;
      }
      hiz_records_.push_back({pk.hiz_key, {pk.hiz_rect[0], pk.hiz_rect[1], pk.hiz_rect[2], pk.hiz_rect[3]},
                              pk.hiz_z, false, pk.vs_hash, pk.ps_hash});
    }
    EmitDraw(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
  }
  const uint32_t emitted_records = uint32_t(hiz_records_.size());
  for (uint32_t index : hiz_held_) {
    const DrawPacket& pk = flushing_[index];
    hiz_records_.push_back({pk.hiz_key, {pk.hiz_rect[0], pk.hiz_rect[1], pk.hiz_rect[2], pk.hiz_rect[3]},
                            pk.hiz_z, true, pk.vs_hash, pk.ps_hash});
  }
  stats_.hiz_held += hiz_held_.size();
  uint32_t base = 0;
  const bool tested = HizRunTest(*depth, uint32_t(hiz_records_.size()), &base);
  // Debug (RENUT_HIZ_VERIFY): each held draw runs whatever the answer, inside an
  // occlusion query, and a draw answered "hidden" that put samples out is a
  // violation.
  const bool verify = tested && hiz_verify_ && hiz_query_pool_ != VK_NULL_HANDLE &&
                      hiz_queries_used_[slot_] + hiz_held_.size() <= kHizMaxQueries;
  uint32_t first_query = 0;
  if (verify && !hiz_held_.empty()) {
    first_query = slot_ * kHizMaxQueries + hiz_queries_used_[slot_];
    vkCmdResetQueryPool(static_cast<VulkanCommandList*>(L())->vk, hiz_query_pool_, first_query,
                        uint32_t(hiz_held_.size()));
    hiz_queries_used_[slot_] += uint32_t(hiz_held_.size());
  }
  if (tested) {
    for (size_t r = 0; r < hiz_records_.size(); ++r) {
      const bool held = hiz_records_[r].held;
      const uint32_t query = verify && held ? first_query + uint32_t(r - emitted_records) : UINT32_MAX;
      hiz_pending_[slot_].push_back({hiz_records_[r].key, held, query, hiz_records_[r].vs, hiz_records_[r].ps});
    }
  } else {
    ++stats_.hiz_test_failed;
  }
  for (size_t k = 0; k < hiz_held_.size(); ++k) {
    DrawPacket pk = flushing_[hiz_held_[k]];
    if (tested && !verify) pk.hiz_pred_offset = (base + emitted_records + uint32_t(k)) * 4;
    if (verify) pk.hiz_query = first_query + uint32_t(k);
    EmitDraw(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
  }
  return true;
}

// The answers of a slot's last frame, once its fence has passed: what each key
// was found to be, for the frames that follow.
void PlumeCommandProcessor::HizReadResults(uint32_t slot) {
  std::vector<HizPending>& pending = hiz_pending_[slot];
  if (hiz_debug_pending_[slot] && hiz_read_[slot]) {
    const auto* all = static_cast<const float*>(hiz_read_[slot]->map());
    if (all) {
      const float* tiles = all + 2 * kHizMaxRecords;
      const uint32_t n = hiz_debug_pending_[slot];
      uint32_t far_tiles = 0;
      float lowest = 1.f, highest = 0.f;
      for (uint32_t i = 0; i < n; ++i) {
        far_tiles += tiles[i] >= 0.9999f;
        lowest = std::min(lowest, tiles[i]);
        highest = std::max(highest, tiles[i]);
      }
      std::string line = fmt::format("Renut: Plume Hi-Z debug: level 0 has {} tiles, {} at the far plane, range {}..{};",
                                     n, far_tiles, lowest, highest);
      for (const auto& r : hiz_debug_rects_[slot]) {
        if (&r - hiz_debug_rects_[slot].data() >= 6) break;
        line += fmt::format(" rect({:.0f},{:.0f})-({:.0f},{:.0f}) z={:.4f}", r[0], r[1], r[2], r[3], r[4]);
      }
      REXLOG_INFO("{}", line);
      hiz_read_[slot]->unmap();
    }
    hiz_debug_pending_[slot] = 0;
  }
  if (pending.empty()) return;
  if (hiz_read_[slot]) {
    const auto* answers = static_cast<const uint32_t*>(hiz_read_[slot]->map());
    if (answers) {
      // One verdict a key per frame: hidden only if every instance was.
      std::unordered_map<uint64_t, bool> hidden;
      std::vector<uint64_t> samples;
      if (hiz_verify_ && hiz_query_pool_ != VK_NULL_HANDLE && hiz_queries_used_[slot]) {
        samples.assign(hiz_queries_used_[slot], 0);
        vkGetQueryPoolResults(static_cast<VulkanDevice*>(device_)->vk, hiz_query_pool_, slot * kHizMaxQueries,
                              hiz_queries_used_[slot], samples.size() * 8, samples.data(), 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      }
      for (size_t i = 0; i < pending.size(); ++i) {
        const bool occluded = answers[i] == 0;
        if (pending[i].query != UINT32_MAX && !samples.empty()) {
          const uint32_t q = pending[i].query - slot * kHizMaxQueries;
          if (q < samples.size()) {
            ++stats_.hiz_verified;
            if (occluded && samples[q]) ++stats_.hiz_violations;
            if (!occluded && !samples[q]) {
              ++stats_.hiz_missed_hidden;
              // Why the test could not tell: no depth there yet, or some.
              const float m = reinterpret_cast<const float*>(answers)[kHizMaxRecords + i];
              if (m >= 0.9999f) ++stats_.hiz_missed_empty;
              auto& pair = hiz_zero_pairs_[pending[i].vs * 0x9E3779B97F4A7C15ull ^ pending[i].ps];
              pair = {pair[0] + 1, pending[i].vs, pending[i].ps};
            }
          }
        }
        auto [it, fresh] = hidden.try_emplace(pending[i].key, occluded);
        if (!fresh) it->second = it->second && occluded;
        ++stats_.hiz_answered;
        ++hiz_total_answered_;
        if (occluded) {
          ++stats_.hiz_occluded;
          ++hiz_total_hidden_;
          if (pending[i].held) ++stats_.hiz_held_skipped;
        } else if (pending[i].held) {
          ++stats_.hiz_held_visible;
        }
      }
      for (const auto& [key, occluded] : hidden) {
        HizHistory& h = hiz_history_[key];
        h.hidden = occluded;
        h.frame = swaps_;
      }
      hiz_read_[slot]->unmap();
    }
  }
  pending.clear();
  hiz_pred_used_[slot] = 0;
  hiz_queries_used_[slot] = 0;
  // What the tests find decides how often to look for more.
  if (hiz_total_answered_ - hiz_gain_answered_ >= 600) {
    const uint64_t answered = hiz_total_answered_ - hiz_gain_answered_;
    const uint64_t hidden = hiz_total_hidden_ - hiz_gain_hidden_;
    // Nothing (under 2%) found hidden: look every 600 frames instead of 8.
    hiz_learn_period_ = hidden * 50 < answered ? 600 : 8;
    hiz_gain_answered_ = hiz_total_answered_;
    hiz_gain_hidden_ = hiz_total_hidden_;
  }
  // Keys that stopped appearing.
  if (hiz_history_.size() > 20000) {
    for (auto it = hiz_history_.begin(); it != hiz_history_.end();) {
      it = swaps_ - it->second.frame > 600 ? hiz_history_.erase(it) : std::next(it);
    }
  }
}

bool PlumeCommandProcessor::ResolveInfoForRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                               draw_util::ResolveInfo& info) {
  if (!resolve_regs_) resolve_regs_ = std::make_unique<RegisterFile>();
  if (!resolve_vertices_) {
    // Guest physical memory the SDK reads the vertices from.
    resolve_vertices_ = rex::system::kernel_memory()->SystemHeapAlloc(64, 16, rex::memory::kSystemHeapPhysical);
    if (!resolve_vertices_) return false;
  }
  const uint32_t physical = memory_->GetPhysicalAddress(resolve_vertices_);
  if (physical == UINT32_MAX) return false;
  const float vertices[6] = {float(x0), float(y0), float(x1), float(y0), float(x0), float(y1)};
  std::memcpy(memory_->TranslatePhysical<uint8_t*>(physical), vertices, sizeof(vertices));

  RegisterFile& regs = *resolve_regs_;
  std::memcpy(regs.values, register_file_->values, sizeof(regs.values));
  xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = physical >> 2;
  fetch.endian = xenos::Endian::kNone;
  fetch.size = 6;
  regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = fetch.dword_0;
  regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1] = fetch.dword_1;
  reg::PA_SU_VTX_CNTL vertex_control;
  vertex_control.value = regs.values[XE_GPU_REG_PA_SU_VTX_CNTL];
  vertex_control.pix_center = xenos::PixelCenter::kOGLHalf;  // no half-pixel offset to undo
  regs.values[XE_GPU_REG_PA_SU_VTX_CNTL] = vertex_control.value;
  reg::PA_SU_SC_MODE_CNTL mode_control;
  mode_control.value = regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  mode_control.vtx_window_offset_enable = 0;
  regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode_control.value;
  reg::PA_SC_WINDOW_SCISSOR_TL window_tl;
  window_tl.value = 0;
  window_tl.window_offset_disable = 1;
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = window_tl.value;
  reg::PA_SC_WINDOW_SCISSOR_BR window_br;
  window_br.value = 0;
  window_br.br_x = 8192;
  window_br.br_y = 8192;
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = window_br.value;
  reg::PA_SC_SCREEN_SCISSOR_TL screen_tl;
  screen_tl.value = 0;
  regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL] = screen_tl.value;
  reg::PA_SC_SCREEN_SCISSOR_BR screen_br;
  screen_br.value = 0;
  screen_br.br_x = 8192;
  screen_br.br_y = 8192;
  regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = screen_br.value;
  return draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info);
}

// A resolve record: the copy registers Direct3D worked out, and the
// rectangle its copy draw would have covered.
void PlumeCommandProcessor::NativeResolve(const uint32_t* w) {
  FlushDeferred();
  for (uint32_t i = 0; i < 12; ++i) WriteRegister(0x2318 + i, w[5 + i]);
  for (uint32_t i = 0; i < 6; ++i) WriteRegister(0x2000 + i, w[17 + i]);
  draw_util::ResolveInfo info;
  if (!ResolveInfoForRect(int32_t(w[1]), int32_t(w[2]), int32_t(w[3]), int32_t(w[4]), info)) {
    ++native_bad_;
    return;
  }
  // The source is the surface bound in the slot the resolve names; its
  // register is RB_COLOR_INFO, _DEPTH_INFO or RB_COLOR1..3_INFO.
  static constexpr uint32_t kRegisterOfSlot[5] = {1, 3, 4, 5, 2};
  // Bit 8 of the source word: Photo Mode's capture routine issued this resolve
  // (native_d3d.cpp), the one copied to the CPU. Not tied to an address: the
  // scratch texture it resolves into is built on the spot.
  capture_this_resolve_ = (w[23] & 0x100) != 0;
  const uint32_t slot = std::min(w[23] & 0xFFu, 4u);
  ResolveSurfaces surfaces;
  surfaces.source = NativeTarget(slot, w[17 + kRegisterOfSlot[slot]]);
  surfaces.depth = SurfaceTarget(4);
  surfaces.x = w[1];
  surfaces.y = w[2];
  surfaces.width = w[3] > w[1] ? w[3] - w[1] : 0;
  surfaces.height = w[4] > w[2] ? w[4] - w[2] : 0;
  surfaces.object_base = w[24];
  surfaces.object_width = w[25];
  surfaces.object_height = w[26];
  surfaces.dest_dx = int32_t(w[27]);
  surfaces.dest_dy = int32_t(w[28]);
  surfaces.object_pitch = w[29];
  ++(surfaces.object_base ? object_resolves_ : address_resolves_count_);
  ExecuteResolve(info, surfaces);
  capture_this_resolve_ = false;
}

bool PlumeCommandProcessor::ExecuteResolve(const draw_util::ResolveInfo& info, const ResolveSurfaces& surfaces) {
  ++stats_.resolves;
  // plume-skip bit 33554432 drops the resolve entirely. Attributing intervals
  // between timestamps proved unreliable on this tiler - the same frame read as
  // 83% resolve or 89% between-passes depending only on which side of the
  // pass-closing barrier the mark sat. Removing a category of work and
  // measuring the difference does not depend on that.
  if (skip_mask_ & 33554432) return true;
  if (!surfaces.width || !surfaces.height) return true;
  const bool copying_depth = info.IsCopyingDepth();
  // The source is the surface the resolve names, the rectangle in its pixels:
  // the host's (a shadow map is drawn smaller than the guest thinks).
  Target* source = surfaces.source;
  // A surface whose image went to an earlier resolve and is read again: what it
  // held comes back first (AliasDepthResolve).
  if (source && source->restore_key) RestoreAliasedDepth(*source, false);
  const uint32_t x = HostFloor(source, surfaces.x), y = HostFloor(source, surfaces.y);
  const uint32_t width = HostCeil(source, surfaces.x + surfaces.width) - x;
  const uint32_t height = HostCeil(source, surfaces.y + surfaces.height) - y;
  if (!width || !height) return true;
  // plume-skip bit 134217728 drops every clear, these and Direct3D's. The
  // picture is wrong; it measures what the clear-only passes cost.
  const bool skip_clears = skip_mask_ & 134217728;
  const uint32_t copy_height = height;
  Trace("resolve {} src=({},{}) {}x{} found={} -> {:08X} dfmt={} ({},{}) pitch={} h={} clear={}{}",
        copying_depth ? "depth" : "color", x, y, width, height,
        source != nullptr, uint32_t(info.copy_dest_base), uint32_t(info.copy_dest_info.copy_dest_format),
        info.copy_dest_coordinate_info.offset_x_div_8 * 8, info.copy_dest_coordinate_info.offset_y_div_8 * 8,
        info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
        info.copy_dest_coordinate_info.height_aligned_div_32 * 32, info.IsClearingColor() ? "c" : "",
        info.IsClearingDepth() ? "d" : "");
  if (source && x < source->width && y < source->height &&
      info.rb_copy_control.copy_command != xenos::CopyCommand::kNull) {
    uint32_t sx = x, sy = y;
    uint32_t w = std::min(width, source->width - x), h = std::min(copy_height, source->height - y);
    const uint32_t dest_w =
        std::max(HostCeil(source, uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32u), w);
    const uint32_t dest_h =
        std::max(HostCeil(source, uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32u), h);
    const uint32_t dx = HostFloor(source, info.copy_dest_coordinate_info.offset_x_div_8 * 8);
    const uint32_t dy = HostFloor(source, info.copy_dest_coordinate_info.offset_y_div_8 * 8);
    const auto* dest_info = rex::graphics::FormatInfo::Get(
        xenos::TextureFormat(uint32_t(info.copy_dest_info.copy_dest_format)));
    const uint32_t dest_bpp = dest_info ? std::max(1u, dest_info->bits_per_pixel / 8) : 4;
    // Depth resolves become R32_FLOAT textures holding the depth, which is what
    // a k_24_8 / k_24_8_FLOAT fetch returns in x (shadow maps, and the scene
    // depth the depth-of-field and fog passes read). The scene's (k_24_8_FLOAT)
    // also keep the stencil in y: the rain ripples read its water bit through
    // an 8_8_8_8 fetch of the same memory.
    const bool with_stencil =
        copying_depth && uint32_t(info.copy_dest_info.copy_dest_format) ==
                             uint32_t(xenos::TextureFormat::k_24_8_FLOAT);
    const RenderFormat dest_format =
        copying_depth ? (with_stencil ? RenderFormat::R32G32_FLOAT : RenderFormat::R32_FLOAT) : source->format;
    // The copy goes into the D3DTexture the resolve names - the level, slice
    // or face Direct3D's own lookup found - at its own size, the source corner
    // landing on the point Direct3D was given; sampled through its fetch
    // constant. A 3D texture's slices interleave: those go by the copy's
    // address and offset.
    const bool to_object = surfaces.object_base != 0;
    Resolved* chosen = nullptr;
    Resolved* aliased = nullptr;
    uint32_t dest_x = 0, dest_y = 0;
    if (to_object) {
      const int32_t scale = int32_t(source->scale);
      int32_t ox = int32_t(sx) + surfaces.dest_dx * scale / 4, oy = int32_t(sy) + surfaces.dest_dy * scale / 4;
      if (ox < 0) {
        const uint32_t cut = std::min(w, uint32_t(-ox));
        sx += cut, w -= cut, ox = 0;
      }
      if (oy < 0) {
        const uint32_t cut = std::min(h, uint32_t(-oy));
        sy += cut, h -= cut, oy = 0;
      }
      const uint32_t object_w = HostCeil(source, std::max(surfaces.object_width, 1u));
      const uint32_t object_h = HostCeil(source, std::max(surfaces.object_height, 1u));
      // A depth map taken whole into a texture of exactly its size (the
      // shadow maps) is not copied: see AliasDepthResolve. Anything else - the
      // scene's depth with its stencil, a part of a surface - copies as before.
      if (depth_alias_ && copying_depth && !with_stencil && source->depth && source->texture &&
          !(skip_mask_ & (268435456 | 536870912)) && sx == 0 && sy == 0 && ox == 0 && oy == 0 &&
          w == source->width && h == source->height && object_w == w && object_h == h) {
        aliased = AliasDepthResolve(*source, surfaces.object_base);
        chosen = aliased;
      }
      if (!chosen) chosen = &GetResolved(surfaces.object_base, dest_format, object_w, object_h);
      dest_x = uint32_t(ox);
      dest_y = uint32_t(oy);
      w = dest_x < chosen->width ? std::min(w, chosen->width - dest_x) : 0;
      h = dest_y < chosen->height ? std::min(h, chosen->height - dest_y) : 0;
    } else {
      chosen = &GetResolved(info.copy_dest_base, dest_format, dest_w + dx, dest_h + dy);
      dest_x = dx;
      dest_y = dy;
    }
    Resolved& dest = *chosen;
    ++dest.generation;
    dest.scale = source->scale;
    dest.depth = copying_depth;
    dest.written = ++resolve_serial_;
    InvalidateResolvedLookups();
    dest.written_frame = swaps_;
    dest.guest_pitch = to_object ? std::max(surfaces.object_pitch, 32u) : dest_w;
    dest.guest_height = to_object ? (std::max(surfaces.object_height, 1u) + 31) & ~31u : dest_h;
    dest.guest_bpp = dest_bpp;
    // Snapshot of the guest memory under the whole resolved texture.
    {
      const uint32_t base_address = to_object ? surfaces.object_base : info.copy_dest_base;
      dest.guest_bytes = dest.guest_pitch * dest.guest_height * dest_bpp;
      dest.guest_hash = ContentHash(memory_->TranslatePhysical<const uint8_t*>(base_address), dest.guest_bytes);
      dest.checked_frame = swaps_;
    }
    if (aliased) {
      // Nothing to copy: the texture is the surface's own image.
    } else if (dest.texture && copying_depth && w && h) {
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      CopyDepth(*source, sx, sy, w, h, dest.texture.get(), dest_x, dest_y, with_stencil && dest.format == dest_format);
    } else if (dest.texture && w && h) {
      Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      // plume-skip bit 268435456 keeps the resolve's barriers and pass breaks
      // but drops the copies themselves, colour and depth (see CopyDepth).
      if (!(skip_mask_ & 268435456)) {
        RenderBox box(int32_t(sx), int32_t(sy), int32_t(sx + w), int32_t(sy + h), 0, 1);
        L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
                                 RenderTextureCopyLocation::Subresource(source->texture.get()), dest_x,
                                 dest_y, 0, &box);
      }
    }
    if (dest.texture) {
      if (snap_address_ && !dest.alias && info.copy_dest_base == snap_address_ && ++snap_seen_ == snap_ordinal_) {
        SnapshotResolved(dest);
      }
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
      if (dest.descriptor) {
        SetTexture2D(dest.descriptor, dest.texture.get());
      }
      const uint32_t base_address = to_object ? surfaces.object_base : info.copy_dest_base;
      if (resolve_log_ && !copying_depth && w >= 64 && h >= 64) {
        LogResolveLine(fmt::format(
            "f={} resolve {}x{} at ({},{}) -> {:08X} {} obj={}x{} pitch={} dfmt={} endian={} swap={} host_fmt={}",
            swaps_, w, h, dest_x, dest_y, base_address, to_object ? "object" : "address", surfaces.object_width,
            surfaces.object_height, dest.guest_pitch, uint32_t(info.copy_dest_info.copy_dest_format),
            uint32_t(info.copy_dest_info.copy_dest_endian), uint32_t(info.copy_dest_info.copy_dest_swap),
            uint32_t(dest.format)));
      }
      // Only the resolve Photo Mode's capture routine issued (capture_this_resolve_): one copy
      // to the CPU per photo, and none while playing.
      if (capture_this_resolve_) {
        ++photo_captures_;
        REXLOG_INFO("Renut: Plume photo capture {}: {}x{} at ({},{}) -> {:08X} {} pitch {} bpp {} format {} depth {}",
                    photo_captures_, w, h, dest_x, dest_y, base_address, to_object ? "object" : "address",
                    dest.guest_pitch, dest_bpp, uint32_t(dest.format), copying_depth);
        if (!copying_depth && w && h && base_address) {
          ReadbackResolve(dest, info, base_address, dest_x, dest_y, w, h, dest.guest_pitch, dest_bpp, source->scale);
        }
      }
    }
  }

  // Clears that ride along with the resolve.
  const RenderRect rect(int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height));
  if (info.IsClearingColor() && source && !skip_clears) {
    Transition(source->texture.get(), source->layout, RenderTextureLayout::COLOR_WRITE);
    std::array<Target*, 4> colors{source};
    SetFramebuffer(GetFramebuffer(colors, 1, nullptr));
    RenderColor color;
    if (source->format == RenderFormat::R8G8B8A8_UNORM) {
      const uint32_t c = info.rb_color_clear;
      color = RenderColor(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f,
                          (c >> 24) / 255.f);
    } else {
      auto half = [](uint32_t h) {
        uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
        float v = exp ? std::ldexp(1.f + mant / 1024.f, int(exp) - 15) : std::ldexp(mant / 1024.f, -14);
        return sign ? -v : v;
      };
      color = RenderColor(half(info.rb_color_clear >> 16), half(info.rb_color_clear & 0xFFFF),
                          half(info.rb_color_clear_lo >> 16), half(info.rb_color_clear_lo & 0xFFFF));
    }
    L()->clearColor(0, color, &rect, 1);
    ++stats_.clears;
  }
  if (info.IsClearingDepth() && !skip_clears) {
    Target* depth = surfaces.depth;
    if (depth) {
      const RenderRect depth_rect(int32_t(HostFloor(depth, surfaces.x)), int32_t(HostFloor(depth, surfaces.y)),
                                  int32_t(HostCeil(depth, surfaces.x + surfaces.width)),
                                  int32_t(HostCeil(depth, surfaces.y + surfaces.height)));
      // A surface whose image went to a resolve: a clear of all of it needs
      // nothing from the old one, a smaller rectangle needs it back.
      if (depth->restore_key || depth->aliased_since_clear || depth->depth_undefined) {
        const bool whole = depth_rect.left <= 0 && depth_rect.top <= 0 &&
                           depth_rect.right >= int32_t(depth->width) && depth_rect.bottom >= int32_t(depth->height);
        NoteAliasedUse(*depth, whole);
        if (depth->restore_key) RestoreAliasedDepth(*depth, whole);
        if (whole) depth->depth_undefined = false;  // this clear takes both aspects
      }
      Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      SetFramebuffer(GetFramebuffer(none, 0, depth));
      const uint32_t d = info.rb_depth_clear;
      L()->clearDepthStencil(true, true, float(d >> 8) / float(0xFFFFFF), d & 0xFF, &depth_rect, 1);
      ++stats_.clears;
    }
  }
  return true;
}

void PlumeCommandProcessor::LogResolveLine(const std::string& line) {
  if (miss_dump_directory_.empty()) return;
  const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
  if (FILE* file = std::fopen((root + "/plume-resolve.log").c_str(), "a")) {
    std::fputs(line.c_str(), file);
    std::fputc('\n', file);
    std::fclose(file);
  }
}

// The CPU reads a few resolves directly: Photo Mode copies the screen into a texture,
// waits for the copy and encodes a JPEG (and the 90x50 thumbnail) from the texels. Plume
// keeps resolves on the GPU, so that read saw whatever the memory held - black. This runs
// the frame so far, waits for it, and writes the pixels into guest memory the way the
// hardware would have: 32x32 tiled, the resolve's endianness and red/blue order.
// The fence the game waits on is recorded in the same list after the resolve, so it is
// not signalled before this returns.
void PlumeCommandProcessor::ReadbackResolve(Resolved& dest, const draw_util::ResolveInfo& info,
                                            uint32_t base_address, uint32_t dest_x, uint32_t dest_y, uint32_t w,
                                            uint32_t h, uint32_t pitch, uint32_t bpp, uint32_t scale) {
  if (bpp != 4 || !dest.texture || !w || !h || !base_address || !scale) return;
  const bool rgba8 = dest.format == RenderFormat::R8G8B8A8_UNORM;
  const bool half = dest.format == RenderFormat::R16G16B16A16_FLOAT;
  if (!rgba8 && !half) {
    REXLOG_INFO("Renut: Plume resolve readback skipped: host format {}", uint32_t(dest.format));
    return;
  }
  const uint32_t texel = rgba8 ? 4 : 8;
  const uint32_t row_texels = (w + 63) & ~63u;
  auto buffer = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(row_texels) * h * texel));
  if (!buffer) return;
  Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_SOURCE);
  RenderBox box(int32_t(dest_x), int32_t(dest_y), int32_t(dest_x + w), int32_t(dest_y + h), 0, 1);
  CopyTextureToBuffer(L(), buffer.get(), dest.texture.get(), w, h, row_texels, 0, 0, &box);
  Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
  Flush();
  const auto* pixels = static_cast<const uint8_t*>(buffer->map());
  if (!pixels) return;
  uint8_t* guest = memory_->TranslatePhysical<uint8_t*>(base_address);
  const bool swap_rb = info.copy_dest_info.copy_dest_swap != 0;
  const uint32_t endian_value = uint32_t(info.copy_dest_info.copy_dest_endian);
  const xenos::Endian endian = endian_value <= 3 ? xenos::Endian(endian_value) : xenos::Endian::k8in32;
  auto to8 = [](uint16_t bits) -> uint32_t {
    const uint32_t exp = (bits >> 10) & 0x1F, mant = bits & 0x3FF;
    if (bits & 0x8000) return 0;
    float v = exp ? std::ldexp(1.f + mant / 1024.f, int(exp) - 15) : std::ldexp(mant / 1024.f, -14);
    if (exp == 31) v = 1.f;
    return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
  };
  uint32_t written = 0;
  const uint32_t limit = dest.guest_bytes;
  // A resolve drawn at a fraction of the guest's resolution is written at the
  // guest's: each guest pixel takes the host texel under it.
  const uint32_t guest_w = uint32_t(uint64_t(w) * 4 / scale), guest_h = uint32_t(uint64_t(h) * 4 / scale);
  const uint32_t guest_x = uint32_t(uint64_t(dest_x) * 4 / scale), guest_y = uint32_t(uint64_t(dest_y) * 4 / scale);
  for (uint32_t gy = 0; gy < guest_h; ++gy) {
    const uint32_t y = std::min(uint32_t(uint64_t(gy) * scale / 4), h - 1);
    const uint8_t* src = pixels + size_t(y) * row_texels * texel;
    for (uint32_t gx = 0; gx < guest_w; ++gx) {
      const uint32_t x = std::min(uint32_t(uint64_t(gx) * scale / 4), w - 1);
      uint32_t r, g, b, a;
      if (rgba8) {
        r = src[x * 4], g = src[x * 4 + 1], b = src[x * 4 + 2], a = src[x * 4 + 3];
      } else {
        const auto* t = reinterpret_cast<const uint16_t*>(src + x * 8);
        r = to8(t[0]), g = to8(t[1]), b = to8(t[2]), a = to8(t[3]);
      }
      if (swap_rb) std::swap(r, b);
      const uint32_t native = r | (g << 8) | (b << 16) | (a << 24);
      const uint32_t value = xenos::GpuSwap(native, endian);
      const uint32_t at = uint32_t(gfx::texture_util::GetTiledOffset2D(int32_t(guest_x + gx), int32_t(guest_y + gy),
                                                                         pitch, 2));
      if (at + 4 > limit) continue;
      std::memcpy(guest + at, &value, 4);
      ++written;
    }
  }
  buffer->unmap();
  // Without this the next lookup would take the changed bytes for another texture
  // streamed into the same memory and drop the resolve.
  dest.guest_hash = ContentHash(memory_->TranslatePhysical<const uint8_t*>(base_address), dest.guest_bytes);
  REXLOG_INFO("Renut: Plume photo capture {} ({}x{} into {:08X}, pitch {}, endian {}, swap {}, {} texels)",
              photo_captures_, w, h, base_address, pitch, endian_value, swap_rb ? 1 : 0, written);
}

bool PlumeCommandProcessor::CreatePackPipeline() {
  if (pack_pipeline_ != VK_NULL_HANDLE) return true;
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = sizeof(k_depth_stencil_pack_comp);
  module_info.pCode = k_depth_stencil_pack_comp;
  if (vkCreateShaderModule(vk_device, &module_info, nullptr, &pack_module_) != VK_SUCCESS) return false;
  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &pack_layout_) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = pack_module_;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = pack_layout_;
  if (vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pack_pipeline_) !=
      VK_SUCCESS) {
    pack_pipeline_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool PlumeCommandProcessor::CreatePatchPipelines() {
  if (patch_pipelines_[1] != VK_NULL_HANDLE) return true;
  if (patch_pipelines_failed_) return false;
  patch_pipelines_failed_ = true;
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  if (patch_layout_ == VK_NULL_HANDLE) {
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &patch_layout_) != VK_SUCCESS) return false;
  }
  const std::pair<const uint32_t*, size_t> code[2] = {{k_patch_levels_comp, sizeof(k_patch_levels_comp)},
                                                      {k_patch_expand_comp, sizeof(k_patch_expand_comp)}};
  for (uint32_t i = 0; i < 2; ++i) {
    if (patch_pipelines_[i] != VK_NULL_HANDLE) continue;
    VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = code[i].second;
    module_info.pCode = code[i].first;
    if (vkCreateShaderModule(vk_device, &module_info, nullptr, &patch_modules_[i]) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = patch_modules_[i];
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = patch_layout_;
    if (vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &patch_pipelines_[i]) !=
        VK_SUCCESS) {
      patch_pipelines_[i] = VK_NULL_HANDLE;
      REXLOG_WARN("Renut: Plume adaptive patch pipelines unavailable; patches stay on the grid");
      return false;
    }
  }
  patch_pipelines_failed_ = false;
  return true;
}

// An adaptive triangle patch draw (the water): N&B works out a factor for
// every edge of every patch on the GPU, from the edge's length on screen and
// its distance, and puts 0 on all three edges of a patch with no point on
// screen - in a lake mostly behind the camera, nearly all of them. Cutting
// every patch at the draw's highest level, as the grid does, ran the domain
// shader on 153 points of each of them instead of 3. The factors stay on the
// GPU, so the GPU lays the draw out too (patch_levels.comp, then
// patch_expand.comp) and the draw reads its index count from what they wrote.
// False leaves the draw to the grid.
bool PlumeCommandProcessor::ExpandAdaptivePatches(uint32_t factors_base, xenos::Endian endian, uint32_t patches,
                                                  uint32_t min_level, uint32_t max_level, AdaptiveDraw& out) {
  if (!CreatePatchPipelines()) return false;
  RenderBuffer* factors = nullptr;
  uint64_t factors_offset = 0;
  if (!BindStream(factors_base, patches * 12, endian, factors, factors_offset) || !factors) return false;
  const uint64_t factors_address = factors->getDeviceAddress() + factors_offset;
  const uint32_t levels = min_level | (max_level << 8);
  Trace("  adaptive patches {} factors {:08X} levels {}..{}", patches, factors_base, min_level, max_level);
  if (patch_reuse_.frame == swaps_ && patch_reuse_.factors == factors_address && patch_reuse_.patches == patches &&
      patch_reuse_.levels == levels && patch_reuse_.exports == export_draws_total_) {
    out = patch_reuse_.draw;
    return true;
  }

  // Room for every patch at the highest level: records, the indirect
  // arguments, the points, then the indices.
  const uint64_t points = uint64_t(patches) * ((max_level + 1) * (max_level + 2) / 2);
  const uint64_t indices = uint64_t(patches) * 3 * max_level * max_level;
  const uint64_t records_bytes = uint64_t(patches) * 16;
  const uint64_t need = records_bytes + 32 + 16 + points * 16 + indices * 4;
  if (need > (256ull << 20)) return false;
  std::unique_ptr<RenderBuffer>& buffer = patch_out_[slot_];
  uint64_t at = (patch_out_used_ + 15) & ~uint64_t(15);
  if (!buffer || at + need > patch_out_capacity_[slot_]) {
    const uint64_t capacity = std::max({need, patch_out_capacity_[slot_] * 2, uint64_t(4) << 20});
    auto made = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        capacity, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::STORAGE |
                      RenderBufferFlag::DEVICE_ADDRESSABLE));  // the layer adds the indirect usage
    if (!made) return false;
    // Draws recorded earlier this frame may still read the old one.
    if (buffer) retired_buffers_[slot_].push_back(std::move(buffer));
    buffer = std::move(made);
    patch_out_capacity_[slot_] = capacity;
    at = 0;
  }
  const uint64_t records_at = at;
  const uint64_t args_at = records_at + records_bytes;
  const uint64_t points_at = (args_at + 32 + 15) & ~uint64_t(15);
  const uint64_t indices_at = points_at + points * 16;
  patch_out_used_ = indices_at + indices * 4;
  const uint64_t base = buffer->getDeviceAddress();

  // Raw Vulkan, outside the render pass; it reopens at the draw.
  auto* vulkan = static_cast<VulkanCommandList*>(L());
  vulkan->endActiveRenderPass();
  VkCommandBuffer cb = vulkan->vk;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  struct {
    uint64_t factors, records, args;
    uint32_t patches, min_level, max_level, first_index;
  } levels_push = {factors_address, base + records_at, base + args_at, patches, min_level, max_level,
                   uint32_t(indices_at / 4)};
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, patch_pipelines_[0]);
  vkCmdPushConstants(cb, patch_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(levels_push), &levels_push);
  vkCmdDispatch(cb, 1, 1, 1);
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                       &barrier, 0, nullptr, 0, nullptr);
  struct {
    uint64_t records, points, indices;
    uint32_t patches, pad;
  } expand_push = {base + records_at, base + points_at, base + indices_at, patches, 0};
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, patch_pipelines_[1]);
  vkCmdPushConstants(cb, patch_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(expand_push), &expand_push);
  vkCmdDispatch(cb, std::min(patches, 65535u), 1, 1);
  // Also orders the reads of the factors before the next export into them.
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
  // The compute layout's push constants may disturb the graphics ones.
  pushed_constants_[0] = pushed_constants_[1] = pushed_constants_[2] = UINT64_MAX;
  ++stats_.adaptive_patch_draws;

  out.buffer = buffer.get();
  out.points_offset = points_at;
  out.points_bytes = points * 16;
  out.args_offset = args_at;
  patch_reuse_.frame = swaps_;
  patch_reuse_.factors = factors_address;
  patch_reuse_.patches = patches;
  patch_reuse_.levels = levels;
  patch_reuse_.exports = export_draws_total_;
  patch_reuse_.draw = out;
  return true;
}

bool PlumeCommandProcessor::CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                      RenderTexture* dest, uint32_t dest_x, uint32_t dest_y, bool with_stencil) {
  // Vulkan copies a depth aspect only to a buffer, so go through one: depth
  // to buffer (4 bytes per texel for D32_SFLOAT), buffer to the colour image.
  // With the stencil, the S8 aspect goes to a second buffer and a compute pass
  // joins the two into RG32F texels (depth_stencil_pack.comp).
  constexpr uint64_t kStagingSize = 4096ull * 2048 * 4;
  if (uint64_t(w) * h * 4 > kStagingSize) return false;
  if (!depth_staging_) {
    depth_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        kStagingSize, RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!depth_staging_) return false;
  }
  if (with_stencil && !stencil_staging_) {
    const auto flags = RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE;
    stencil_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize / 4, flags));
    packed_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize * 2, flags));
    if (!stencil_staging_ || !packed_staging_ || !CreatePackPipeline()) {
      REXLOG_WARN("Renut: Plume cannot keep the stencil of depth resolves");
      stencil_staging_.reset();
      packed_staging_.reset();
    }
  }
  with_stencil = with_stencil && packed_staging_ && pack_pipeline_ != VK_NULL_HANDLE;
  // Always a barrier call, which also ends any open render pass before the
  // raw Vulkan commands below.
  L()->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(source.texture.get(), RenderTextureLayout::COPY_SOURCE));
  ++stats_.barriers;
  source.layout = RenderTextureLayout::COPY_SOURCE;
  bound_framebuffer_ = nullptr;
  // plume-skip bit 536870912 keeps that barrier and drops the round trip
  // through the staging buffer, to price it; 268435456 drops every copy.
  if (skip_mask_ & (268435456 | 536870912)) return true;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(L())->vk;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {int32_t(x), int32_t(y), 0};
  region.imageExtent = {w, h, 1};
  VkImage image = static_cast<VulkanTexture*>(source.texture.get())->vk;
  vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<VulkanBuffer*>(depth_staging_.get())->vk, 1, &region);
  if (with_stencil) {
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(stencil_staging_.get())->vk, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    struct {
      uint64_t depth, stencil, packed;
      uint32_t count, pad;
    } push = {depth_staging_->getDeviceAddress(), stencil_staging_->getDeviceAddress(),
              packed_staging_->getDeviceAddress(), w * h, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pack_pipeline_);
    vkCmdPushConstants(cb, pack_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (w * h + 63) / 64, 1, 1);
    // The compute layout's push constants may disturb the graphics ones.
    pushed_constants_[0] = pushed_constants_[1] = pushed_constants_[2] = UINT64_MAX;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                             RenderTextureCopyLocation::PlacedFootprint(packed_staging_.get(),
                                                                        RenderFormat::R32G32_FLOAT, w, h, 1, w),
                             dest_x, dest_y);
    return true;
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                           RenderTextureCopyLocation::PlacedFootprint(depth_staging_.get(),
                                                                      RenderFormat::R32_FLOAT, w, h, 1, w),
                           dest_x, dest_y);
  return true;
}

void PlumeCommandProcessor::SnapshotResolved(Resolved& dest) {
  if (!snap_ || snap_width_ != dest.width || snap_height_ != dest.height || snap_format_ != dest.format) {
    snap_ = device_->createTexture(RenderTextureDesc::Texture2D(dest.width, dest.height, 1, dest.format));
    snap_width_ = dest.width;
    snap_height_ = dest.height;
    snap_format_ = dest.format;
    snap_layout_ = RenderTextureLayout::UNKNOWN;
  }
  if (snap_) {
    Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_SOURCE);
    Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_DEST);
    L()->copyTexture(snap_.get(), dest.texture.get());
    snap_taken_ = true;
  }
}


// Puts every image a frame wrote into one known layout before the frame ends.
//
// Two frames in flight share these textures, and Transition only emits a
// barrier when the layout it tracks changes - so without this the next frame
// would write a target the last frame wrote with no image barrier in between.
// An execution barrier is not enough: these are UBWC-compressed on an Adreno,
// and writing one from two submissions with no layout transition between them
// leaves the compression metadata inconsistent, which shows up as a blocky,
// darkened frame. Landing on SHADER_READ here guarantees the next frame's first
// use transitions back, and a layout transition preserves the contents.
void PlumeCommandProcessor::FlushImagesForNextFrame() {
  for (auto& [key, target] : targets_) {
    if (target.texture) Transition(target.texture.get(), target.layout, RenderTextureLayout::SHADER_READ);
  }
  for (auto& [key, r] : resolved_) {
    if (r.texture) Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
    if (r.fitted) Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::SHADER_READ);
  }
}

void PlumeCommandProcessor::LogSkinned() {
  std::lock_guard<std::mutex> lock(skinned_mutex_);
  REXLOG_ERROR("Renut: Plume skinned vertex shaders seen: {}", skinned_seen_.size());
  for (const auto& [hash, seen] : skinned_seen_) {
    REXLOG_ERROR("Renut: Plume   skinned vs={:016X} draws={} frames {}..{}", hash, seen.draws, seen.first_frame,
                 seen.last_frame);
  }
}

void PlumeCommandProcessor::ValidateRange(const char* what, uint64_t draw_hash, RenderBuffer* buffer, uint64_t offset,
                                          uint32_t guest_address, uint32_t size, xenos::Endian endian) {
  if (!buffer || buffer == upload_.get() || !size) return;
  std::vector<uint8_t> guest(size);
  CopySwapped(guest.data(), memory_->TranslatePhysical<const uint8_t*>(guest_address), size, endian);
  const auto* window = static_cast<const uint8_t*>(buffer->map());
  if (!window) return;
  uint32_t differ = 0, first = UINT32_MAX;
  for (uint32_t i = 0; i < size; ++i) {
    if (window[offset + i] != guest[i]) {
      if (first == UINT32_MAX) first = i;
      ++differ;
    }
  }
  buffer->unmap();
  ++validate_ranges_;
  if (!differ) return;
  // A write that landed since the window was made has already been recorded (the callback runs before
  // the write completes); the next drain drops the window. Only a difference with nothing pending counts.
  const uint64_t drops_before = kept_drop_events_;
  DrainWatchHits();
  if (kept_drop_events_ != drops_before) return;
  ++validate_stale_;
  if (validate_logged_ < 120) {
    ++validate_logged_;
    REXLOG_ERROR("Renut: Plume STALE {} at draw: frame {} vs/ps hash {:016X} guest {:08X}+{:X}: {} bytes differ, first at +{:X}",
                 what, swaps_, draw_hash, guest_address, size, differ, first);
  }
}

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  auto* renderer = PlumeRenderer::Get();
  ExecutorBusy busy;
  if (native_on_) {
    // Everything the lists hold for this frame, before it is shown (from the
    // ring's swap; a present the lists hold has run what came before it).
    if (!native_presenting_) RunNative(UINT32_MAX);
    EvictSurfaceTargets();
    native_d3d::NativeReport(swaps_, native_draws_, native_ring_draws_dropped_, native_bad_,
                             [this] { return SurfaceReport(); });
  }
  FlushDeferred();
  if (skin_log_ && swaps_ == 930) LogSkinned();
  if (validate_windows_ && swaps_ % 60 == 0) {
    REXLOG_INFO("Renut: Plume validation at swap {}: {} ranges compared, {} stale", swaps_, validate_ranges_, validate_stale_);
  }
  // Before anything is picked for presenting: the one that is moves on to
  // COPY_SOURCE below, which is a transition of its own.
  FlushImagesForNextFrame();
  RenderTexture* source = nullptr;
  Resolved* found = FindResolved(frontbuffer_ptr);
  if (resolve_log_) {
    LogResolveLine(fmt::format("f={} swap front={:08X} {}x{} found={}", swaps_, frontbuffer_ptr, frontbuffer_width,
                               frontbuffer_height, found != nullptr));
  }
  if (found && found->texture) {
    Resolved& r = *found;
    Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
    source = r.texture.get();
    frontbuffer_width = std::min(frontbuffer_width, r.width);
    frontbuffer_height = std::min(frontbuffer_height, r.height);
  }
  bool snap_dump = false;
  if (snap_taken_ && snap_) {
    Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_SOURCE);
    source = snap_.get();
    frontbuffer_width = std::min(frontbuffer_width, snap_width_);
    frontbuffer_height = std::min(frontbuffer_height, snap_height_);
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (!miss_dump_directory_.empty() && access((root + "/plume-snap-dump").c_str(), F_OK) == 0) {
      snap_dump_row_ = (snap_width_ + 63) & ~63u;
      snap_dump_ = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(snap_dump_row_) * snap_height_ * 4));
      if (snap_dump_) {
        CopyTextureToBuffer(L(), snap_dump_.get(), snap_.get(), snap_width_, snap_height_, snap_dump_row_, 0, 0,
                            nullptr);
        snap_dump = true;
      }
    }
  }
  snap_taken_ = false;
  snap_seen_ = 0;
  frame_draw_ = 0;
  // A traced frame also copies every exported range aside, written out below
  // once the frame has run, next to the guest bytes it started from.
  std::unique_ptr<RenderBuffer> export_dump;
  std::vector<std::pair<uint64_t, const ExportRange*>> export_dump_at;
  if (trace_ && !exports_.empty()) {
    uint64_t total = 0;
    for (const auto& [key, range] : exports_) total += range.size;
    if (total <= (16u << 20)) export_dump = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(total));
    if (export_dump) {
      ExportBarrier();
      uint64_t at = 0;
      for (const auto& [key, range] : exports_) {
        L()->copyBufferRegion(RenderBufferReference(export_dump.get(), at),
                                RenderBufferReference(range.buffer.get(), 0), range.size);
        export_dump_at.emplace_back(at, &range);
        at += range.size;
      }
    }
  }
  RecordWritebacks();
  // FlushImagesForNextFrame already closed any open pass with its barriers.
  WriteTimestamp();
  timestamp_count_[slot_] = timestamps_used_;
  pair_count_[slot_] = pair_used_;
  pair_pending_[slot_] = pair_capture_;
  pair_capture_ = false;
  L()->end();
  list_open_ = false;
  const auto submit_start = std::chrono::steady_clock::now();
  if (renderer) {
    renderer->SubmitAndPresent(list_, source, frontbuffer_width, frontbuffer_height, slot_);
    if (writeback_ && !writeback_fast_ && !writebacks_[slot_].empty()) {
      renderer->WaitForSlot(slot_);
      ApplyWritebacks(slot_);
    }
  }
  const auto submit_end = std::chrono::steady_clock::now();
  stats_.submit_us +=
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count());
  if (frame_start_.time_since_epoch().count()) {
    stats_.frame_us += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(submit_end - frame_start_).count());
  }
  frame_start_ = submit_end;
  // The frame is on its way: hold the command thread, and with it the game
  // behind its fences, to the rate the player chose. While the vblank holds the
  // game (its own 30 FPS and an interval that waits for it) that is its job, and
  // unlimited has nothing to wait for; both make this a no-op.
  {
    static pacing::Pacer pacer;
    pacer.Wait(pacing::VblankPaces() ? 0 : pacing::Target());
  }
  if (snap_dump && renderer) {
    renderer->WaitForAllSlots();
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (FILE* file = std::fopen((root + "/plume-snap.rgba").c_str(), "wb")) {
      std::fprintf(file, "%u %u %u %u\n", snap_width_, snap_height_, snap_dump_row_, uint32_t(snap_format_));
      std::fwrite(snap_dump_->map(), 4, size_t(snap_dump_row_) * snap_height_, file);
      snap_dump_->unmap();
      std::fclose(file);
    }
    std::remove((root + "/plume-snap-dump").c_str());
    if (snap_shader_) {
      std::remove((root + "/plume-snap-shader").c_str());
      snap_shader_ = 0;
    }
    snap_dump_.reset();
    REXLOG_INFO("Renut: Plume snapshot written ({}x{}, format {})", snap_width_, snap_height_, uint32_t(snap_format_));
  }
  if (depth_dump_ && renderer) {
    renderer->WaitForAllSlots();
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    const uint32_t bytes = depth_dump_format_ == uint32_t(RenderFormat::R32G32_FLOAT) ? 8 : 4;
    if (FILE* file = std::fopen((root + "/plume-depth.f32").c_str(), "wb")) {
      std::fprintf(file, "%u %u %u %u %llu %llu\n", depth_dump_w_, depth_dump_h_, depth_dump_row_, bytes,
                   (unsigned long long)depth_dump_generation_, (unsigned long long)depth_dump_frame_);
      std::fwrite(depth_dump_->map(), 1, size_t(depth_dump_row_) * depth_dump_h_ * bytes, file);
      depth_dump_->unmap();
      std::fclose(file);
    }
    std::remove((root + "/plume-depth-dump").c_str());
    depth_dump_ps_ = 0;
    depth_dump_.reset();
    REXLOG_INFO("Renut: Plume depth resolve written ({}x{}, {} bytes a texel)", depth_dump_w_, depth_dump_h_, bytes);
  }
  const uint64_t cpu_now = ThreadCpuMicroseconds();
  if (cpu_at_swap_) stats_.cp_cpu_us += cpu_now - cpu_at_swap_;
  cpu_at_swap_ = cpu_now;
  if (trace_) {
    Trace("swap front={:08X} {}x{} found={}", frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
          source != nullptr);
    for (auto& [key, target] : targets_) {
      Trace("  target base={:X} fmt={} depth={} {}x{}", key & 0xFFF, (key >> 12) & 0xF, (key >> 16) & 1,
            target.width, target.height);
    }
    for (const auto& [key, r] : resolved_) {
      Trace("  resolved {:08X} host={}x{} f{} guest={}x{}/{} gen={} frame={}", uint32_t(key),
            r.width, r.height, uint32_t(r.format), r.guest_pitch, r.guest_height, r.guest_bpp,
            r.generation, r.written_frame);
    }
    for (const TracedStream& t : traced_streams_) {
      const auto* now = memory_->TranslatePhysical<const uint8_t*>(t.address);
      uint32_t changed = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < t.bytes.size(); i += 4) {
        if (std::memcmp(now + i, t.bytes.data() + i, 4)) {
          ++changed;
          first = std::min(first, i);
        }
      }
      if (changed) {
        Trace("  stale draw #{} stream{} {:08X}+{:X}: {} of {} dwords changed since, first at +{:X}", t.draw,
              t.stream, t.address, t.bytes.size(), changed, t.bytes.size() / 4, first);
      }
    }
    traced_streams_.clear();
    std::fclose(trace_);
    trace_ = nullptr;
    traced_cascade_ps_.clear();
    REXLOG_INFO("Renut: Plume frame trace written");
    if (export_dump && renderer && !miss_dump_directory_.empty()) {
      renderer->WaitForAllSlots();
      const std::string directory =
          miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/" + trace_dump_name_;
      renutMakeDirectory(directory);
      const auto* data = static_cast<const uint8_t*>(export_dump->map());
      for (const auto& [at, range] : export_dump_at) {
        const std::string name = fmt::format("{}/{:08X}", directory, range->base);
        if (FILE* file = std::fopen((name + ".gpu").c_str(), "wb")) {
          std::fwrite(data + at, 1, range->size, file);
          std::fclose(file);
        }
        if (FILE* file = std::fopen((name + ".guest").c_str(), "wb")) {
          std::fwrite(memory_->TranslatePhysical<const uint8_t*>(range->base), 1, range->size, file);
          std::fclose(file);
        }
      }
      export_dump->unmap();
      REXLOG_INFO("Renut: Plume wrote {} exported ranges", export_dump_at.size());
    }
  }
  ++swaps_;
  if (!pending_pipelines_.empty()) ReapPipelines();
  lookup_spent_us_ = 0;
  pipeline_frame_.store(swaps_, std::memory_order_relaxed);
  if (!miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (swaps_ % 30 == 0) {
      const bool arm = access((root + "/plume-trace-arm").c_str(), F_OK) == 0;
      if (arm && !arm_) {
        arm_pairs_.clear();
        arm_target_ = 0;
        if (FILE* file = std::fopen((root + "/plume-trace-arm").c_str(), "r")) {
          unsigned long long target = 0;
          if (std::fscanf(file, "%llx", &target) == 1) arm_target_ = target;
          std::fclose(file);
        }
        arm_learn_ = arm_target_ ? 0 : 150;
        arm_burst_ = arm_left_ = 0;
        if (arm_target_) {
          REXLOG_INFO("Renut: Plume trace armed on shader {:016X}", arm_target_);
        } else {
          REXLOG_INFO("Renut: Plume trace armed, learning the scene's shaders");
        }
      }
      arm_ = arm;
    }
    if (arm_learn_ && !--arm_learn_) REXLOG_INFO("Renut: Plume trace armed with {} shader pairs", arm_pairs_.size());
    if (arm_ && !trace_ && !arm_left_ && !arm_new_.empty() && arm_burst_ < 6) {
      ++arm_burst_;
      arm_left_ = 3;
      for (const auto& [vs_hash, ps_hash] : arm_new_) {
        REXLOG_INFO("Renut: Plume trace burst {} on new pair vs={:016X} ps={:016X}", arm_burst_, vs_hash,
                    ps_hash);
      }
    }
    arm_new_.clear();
    if (arm_left_ && !trace_) {
      const std::string name = fmt::format("{}/plume-trace-arm-{}-{}.txt", root, arm_burst_, 4 - arm_left_);
      trace_ = std::fopen(name.c_str(), "w");
      trace_dump_name_ = fmt::format("exportdump-arm-{}-{}", arm_burst_, 4 - arm_left_);
      --arm_left_;
    }
  }
  if (!miss_dump_directory_.empty() && !trace_) {
    // Every frame: a request made when something looks wrong has to catch the
    // frame that shows it.
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (std::remove((root + "/plume-trace-request").c_str()) == 0) {
      trace_ = std::fopen((root + "/plume-trace.txt").c_str(), "w");
      trace_dump_name_ = "exportdump";
    }
  }
  if (swaps_ % 30 == 0 && !miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    texdump_directory_.clear();
    if (std::remove((root + "/plume-texdump-request").c_str()) == 0) {
      texdump_directory_ = root + "/texdump";
      renutMakeDirectory(texdump_directory_);
      dumped_.clear();
      REXLOG_INFO("Renut: Plume dumping the next frame's textures");
    }
  }
  if (swaps_ % 30 == 0 && !miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    uint32_t mask = 0;
    uint32_t bind_skip = 0;
    bool shadow_native_constants = true;
    if (FILE* file = std::fopen((root + "/plume-shadow-constants").c_str(), "r")) {
      int value = 1;
      if (std::fscanf(file, "%d", &value) == 1) shadow_native_constants = value != 0;
      std::fclose(file);
    }
    if (shadow_native_constants != shadow_native_constants_) {
      REXLOG_INFO("Renut: Plume shadow PS constants {}", shadow_native_constants ? "native skip" : "legacy upload");
    }
    shadow_native_constants_ = shadow_native_constants;
    if (FILE* file = std::fopen((root + "/plume-bind").c_str(), "r")) {
      if (std::fscanf(file, "%u", &bind_skip) != 1) bind_skip = 0;
      std::fclose(file);
    }
    if (bind_skip != bind_skip_) REXLOG_INFO("Renut: Plume bind skip {}", bind_skip);
    bind_skip_ = bind_skip;
    {
      int defer = std::clamp(REXCVAR_GET(renut_plume_defer_draws), 0, 2);
      if (FILE* file = std::fopen((root + "/plume-defer").c_str(), "r")) {
        int value = defer;
        if (std::fscanf(file, "%d", &value) == 1) defer = std::clamp(value, 0, 2);
        std::fclose(file);
      }
      if (uint32_t(defer) != defer_mode_) {
        REXLOG_INFO("Renut: Plume deferred draws mode {} (0 off, 1 depth-only, 2 opaque colour too)", defer);
      }
      defer_mode_ = uint32_t(defer);
    }
    {
      bool trail = trail_device_default_;
      if (FILE* file = std::fopen((root + "/plume-trail").c_str(), "r")) {
        int value = trail;
        if (std::fscanf(file, "%d", &value) == 1) trail = value != 0;
        std::fclose(file);
      }
      if (trail != trail_) REXLOG_INFO("Renut: Plume draw trail {}", trail ? "on" : "off");
      trail_ = trail;
    }
    {
      // files/plume-recthread: 0 records the draws on this thread, as before; 1 (the
      // default) hands them to the recorder (see RecQueue).
      bool recorder = true;
      if (FILE* file = std::fopen((root + "/plume-recthread").c_str(), "r")) {
        int value = 1;
        if (std::fscanf(file, "%d", &value) == 1) recorder = value != 0;
        std::fclose(file);
      }
      if (recorder != rec_enabled_) {
        REXLOG_INFO("Renut: Plume recording thread {}", recorder ? "on" : "off");
        SyncRecorder();
      }
      rec_enabled_ = recorder;
    }
    {
      // Resolve textures, framebuffers and streams by what the guest bound
      // rather than by hashing it again at every draw. 0 puts the old path back.
      bool objkeys = true;
      if (FILE* file = std::fopen((root + "/plume-objkeys").c_str(), "r")) {
        int value = 1;
        if (std::fscanf(file, "%d", &value) == 1) objkeys = value != 0;
        std::fclose(file);
      }
      if (objkeys != objkeys_) {
        REXLOG_INFO("Renut: Plume object keys {}", objkeys ? "on" : "off");
        ++object_epoch_;
        ++stream_epoch_;
      }
      objkeys_ = objkeys;
    }
    {
      // files/plume-shadercache and files/plume-keyreuse: 0 puts the old path
      // back, 1 forces it on; absent leaves the cvar's choice.
      auto read_switch = [&](const char* name, bool fallback) {
        bool value = fallback;
        if (FILE* file = std::fopen((root + "/" + name).c_str(), "r")) {
          int v = fallback;
          if (std::fscanf(file, "%d", &v) == 1) value = v != 0;
          std::fclose(file);
        }
        return value;
      };
      const bool shader_cache = read_switch("plume-shadercache", REXCVAR_GET(renut_plume_shader_cache));
      if (shader_cache != shader_cache_) {
        REXLOG_INFO("Renut: Plume shader cache {}", shader_cache ? "on" : "off");
        shader_watch_.clear();
        shader_watch_memo_.Clear();
        shader_block_index_.clear();
      }
      shader_cache_ = shader_cache;
      const bool key_reuse = read_switch("plume-keyreuse", REXCVAR_GET(renut_plume_key_reuse));
      if (key_reuse != key_reuse_) {
        REXLOG_INFO("Renut: Plume pipeline key reuse {}", key_reuse ? "on" : "off");
        // Keys made the other way name different pipelines: the memos go, the
        // pipelines are found again (or made) under the new keys.
        pipeline_memo_.Clear();
      }
      key_reuse_ = key_reuse;
    }
    {
      int hiz = std::clamp(REXCVAR_GET(renut_plume_hiz), 0, 4);
      if (FILE* file = std::fopen((root + "/plume-hiz").c_str(), "r")) {
        int value = hiz;
        if (std::fscanf(file, "%d", &value) == 1) hiz = std::clamp(value, 0, 4);
        std::fclose(file);
      }
      if (uint32_t(hiz) != hiz_mode_) {
        REXLOG_INFO("Renut: Plume Hi-Z mode {} (0 off, 1 counts, 2 frustum/scissor, 3 + occlusion, 4 + occlusion of depth-only draws)", hiz);
      }
      hiz_mode_ = uint32_t(hiz);
      bool skip = true;
      if (FILE* file = std::fopen((root + "/plume-hizskip").c_str(), "r")) {
        int value = 1;
        if (std::fscanf(file, "%d", &value) == 1) skip = value != 0;
        std::fclose(file);
      }
      hiz_skip_ = skip;
    }
    {
      int fetch_spec = std::clamp(REXCVAR_GET(renut_plume_fetch_spec), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-fetchspec").c_str(), "r")) {
        int value = fetch_spec;
        if (std::fscanf(file, "%d", &value) == 1) fetch_spec = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(fetch_spec) != fetch_spec_mode_) {
        REXLOG_INFO("Renut: Plume live fetch table as specialization constants {}", fetch_spec ? "on" : "off");
      }
      fetch_spec_mode_ = uint32_t(fetch_spec);
    }
    {
      // 1 (default): a shadow map resolved whole is sampled in place, not copied.
      int alias = 1;
      if (FILE* file = std::fopen((root + "/plume-depthalias").c_str(), "r")) {
        int value = alias;
        if (std::fscanf(file, "%d", &value) == 1) alias = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if ((alias != 0) != depth_alias_) {
        REXLOG_INFO("Renut: Plume depth maps resolved {}", alias ? "in place (no copy)" : "by copy");
      }
      depth_alias_ = alias != 0;
    }
    {
      int bc = std::clamp(REXCVAR_GET(renut_plume_bc_textures), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-bc").c_str(), "r")) {
        int value = bc;
        if (std::fscanf(file, "%d", &value) == 1) bc = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(bc) != bc_mode_) {
        REXLOG_INFO("Renut: Plume BC textures {}: the 2D guest textures are uploaded again", bc ? "on" : "off");
        bc_mode_ = uint32_t(bc);
        // Every 2D texture is rebuilt in the other form the next time it is drawn.
        for (auto& [key, texture] : guest_textures_) RetireGuestTexture(texture);
        guest_textures_.clear();
        texture_memo_.Clear();
        ++object_epoch_;
      }
    }
    {
      int quality = std::clamp(REXCVAR_GET(renut_plume_texture_quality), 0, 2);
      if (FILE* file = std::fopen((root + "/plume-texquality").c_str(), "r")) {
        int value = quality;
        if (std::fscanf(file, "%d", &value) == 1) quality = std::clamp(value, 0, 2);
        std::fclose(file);
      }
      if (uint32_t(quality) != texture_quality_) {
        REXLOG_INFO("Renut: Plume texture quality {} (each texture is uploaded again when it is next drawn)",
                    quality == 0 ? "full" : quality == 1 ? "half" : "quarter");
        texture_quality_ = uint32_t(quality);
      }
    }
    {
      int rel = std::clamp(REXCVAR_GET(renut_plume_rel_constants), 0, 2);
      if (FILE* file = std::fopen((root + "/plume-relconst").c_str(), "r")) {
        int value = rel;
        if (std::fscanf(file, "%d", &value) == 1) rel = (value >= 0 && value <= 2) || value == 11 || value == 12 ? value : 0;
        std::fclose(file);
      }
      if (uint32_t(rel) != rel_constants_mode_) {
        REXLOG_INFO("Renut: Plume relative vertex constants mode {} (0 four blocks, 1 one block, 2 one block loaded, "
                    "11/12 alternate 0 with 1/2 per draw)", rel);
      }
      rel_constants_mode_ = uint32_t(rel);
    }
    {
      int depth_strip = std::clamp(REXCVAR_GET(renut_plume_depth_strip), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-depthstrip").c_str(), "r")) {
        int value = depth_strip;
        if (std::fscanf(file, "%d", &value) == 1) depth_strip = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(depth_strip) != depth_strip_mode_) {
        REXLOG_INFO("Renut: Plume depth-only draws without vertex outputs {}", depth_strip ? "on" : "off");
      }
      depth_strip_mode_ = uint32_t(depth_strip);
    }
    {
      int adaptive = std::clamp(REXCVAR_GET(renut_plume_adaptive_patches), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-adaptive").c_str(), "r")) {
        int value = adaptive;
        if (std::fscanf(file, "%d", &value) == 1) adaptive = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(adaptive) != adaptive_mode_) {
        REXLOG_INFO("Renut: Plume adaptive patches {}", adaptive ? "on" : "off");
      }
      adaptive_mode_ = uint32_t(adaptive);
    }
    {
      unsigned int percent = 100;
      if (FILE* file = std::fopen((root + "/plume-patchlevel").c_str(), "r")) {
        if (std::fscanf(file, "%u", &percent) != 1) percent = 100;
        std::fclose(file);
      }
      percent = std::clamp(percent, 10u, 100u);
      if (percent != patch_level_percent_) REXLOG_INFO("Renut: Plume water tessellation at {}%", percent);
      patch_level_percent_ = percent;
    }
    {
      unsigned int minimum = no_shadows_ ? UINT32_MAX : 0;
      if (!no_shadows_) if (FILE* file = std::fopen((root + "/plume-shadowmin").c_str(), "r")) {
        if (std::fscanf(file, "%u", &minimum) != 1) minimum = 0;
        std::fclose(file);
      }
      if (minimum != shadow_min_indices_) REXLOG_INFO("Renut: Plume shadow casters under {} indices dropped", minimum);
      shadow_min_indices_ = minimum;
    }
    // Arms a per-pair GPU profile of the next few frames (see PairBegin). The
    // file is removed as it is read, so one write is one capture.
    if (FILE* file = std::fopen((root + "/plume-pairprof").c_str(), "r")) {
      int mode = 1, frames = 3;
      const int read = std::fscanf(file, "%d %d", &mode, &frames);
      std::fclose(file);
      std::remove((root + "/plume-pairprof").c_str());
      if (read >= 1 && mode >= 1 && pair_pool_ != VK_NULL_HANDLE && pairprof_frames_left_ == 0) {
        pairprof_mode_ = uint32_t(std::min(mode, 2));
        pairprof_frames_left_ = uint32_t(std::clamp(read >= 2 ? frames : 3, 1, 16));
        pair_totals_.clear();
        pair_frames_ = 0;
        pair_span_ns_ = pair_gap_ns_ = pair_frame_ns_ = 0;
        REXLOG_INFO("Renut: Plume pair profile: mode {}, {} frames", pairprof_mode_, pairprof_frames_left_);
      }
    }
    {
      bool pass_all = false;
      if (FILE* file = std::fopen((root + "/plume-pred-all").c_str(), "r")) {
        int value = 0;
        if (std::fscanf(file, "%d", &value) == 1) pass_all = value != 0;
        std::fclose(file);
      }
      if (pass_all != pred_pass_all_) REXLOG_INFO("Renut: predicated packets {}", pass_all ? "all run" : "bin-checked");
      pred_pass_all_ = pass_all;
      bool fragpos = false;
      if (FILE* file = std::fopen((root + "/plume-band-fragpos").c_str(), "r")) {
        int value = 0;
        if (std::fscanf(file, "%d", &value) == 1) fragpos = value != 0;
        std::fclose(file);
      }
      if (fragpos != band_fragpos_) REXLOG_INFO("Renut: band-relative pixel positions {}", fragpos ? "on" : "off");
      band_fragpos_ = fragpos;
    }
    {
      int split_mode = 2;
      if (FILE* file = std::fopen((root + "/plume-keep-split").c_str(), "r")) {
        if (std::fscanf(file, "%d", &split_mode) != 1) split_mode = 2;
        std::fclose(file);
      }
      if (split_mode != g_renut_scene_split_mode.exchange(split_mode))
        REXLOG_INFO("Renut: scene band split mode {}", split_mode);
    }
    if (FILE* file = std::fopen((root + "/plume-skip").c_str(), "r")) {
      if (std::fscanf(file, "%u", &mask) != 1) mask = 0;
      std::fclose(file);
    }
    if (mask != skip_mask_) {
      REXLOG_INFO("Renut: Plume skip mask {:X}", mask);
      // Two of its bits change how a level's layout is worked out.
      fetch_layouts_.clear();
      layout_memo_.Clear();
      ++object_epoch_;
    }
    skip_mask_ = mask;
    const bool sync_draws = access((root + "/plume-syncdraws").c_str(), F_OK) == 0;
    if (sync_draws != sync_draws_) REXLOG_INFO("Renut: Plume synced draws {}", sync_draws ? "on" : "off");
    sync_draws_ = sync_draws;
    {
      unsigned every = 0;
      if (FILE* file = std::fopen((root + "/plume-syncevery").c_str(), "r")) {
        if (std::fscanf(file, "%u", &every) != 1) every = 0;
        std::fclose(file);
      }
      if (every != sync_every_) REXLOG_INFO("Renut: Plume list submitted and waited for every {} draws", every);
      sync_every_ = every;
    }
    {
      unsigned long long watch = 0;
      if (FILE* file = std::fopen((root + "/plume-syncwatch").c_str(), "r")) {
        if (std::fscanf(file, "%llx", &watch) != 1) watch = 0;
        std::fclose(file);
      }
      sync_watch_ = watch;
      unsigned long long delay = 0;
      unsigned mask = 15;
      if (FILE* file = std::fopen((root + "/plume-retire-delay").c_str(), "r")) {
        if (std::fscanf(file, "%llu %u", &delay, &mask) < 1) delay = 0;
        std::fclose(file);
      }
      if (delay != retire_delay_ || mask != retire_mask_) {
        REXLOG_INFO("Renut: Plume retired objects (classes {:X}) kept {} frames", mask, delay);
      }
      retire_delay_ = delay;
      retire_mask_ = mask;
    }
    const bool keep_streams = access((root + "/plume-no-persist").c_str(), F_OK) != 0;
    if (keep_streams != keep_streams_) {
      REXLOG_INFO("Renut: Plume kept streams {}", keep_streams ? "on" : "off");
      if (!keep_streams) {
        for (auto& [key, kept] : kept_streams_) {
          if (kept.buffer) retired_buffers_[slot_].push_back(std::move(kept.buffer));
        }
        ++stream_epoch_;
    kept_streams_.clear();
  kept_memo_.Clear();
        kept_bytes_ = 0;
      }
    }
    keep_streams_ = keep_streams;
    const bool writeback = REXCVAR_GET(renut_plume_memexport_readback) ||
        access((root + "/plume-writeback").c_str(), F_OK) == 0;
    if (writeback != writeback_) {
      REXLOG_INFO("Renut: Plume export writeback {}", writeback ? "on" : "off");
    }
    writeback_ = writeback;
    writeback_fast_ = REXCVAR_GET(renut_plume_memexport_fast);
    uint32_t address = 0, ordinal = 0;
    if (FILE* file = std::fopen((root + "/plume-snap").c_str(), "r")) {
      if (std::fscanf(file, "%x %u", &address, &ordinal) != 2) address = ordinal = 0;
      std::fclose(file);
    }
    if (address != snap_address_ || ordinal != snap_ordinal_) {
      REXLOG_INFO("Renut: Plume snapshot of resolve {} into {:08X}", ordinal, address);
    }
    snap_address_ = address;
    snap_ordinal_ = ordinal;
    unsigned long long snap_shader = 0;
    if (FILE* file = std::fopen((root + "/plume-snap-shader").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &snap_shader) != 1) snap_shader = 0;
      std::fclose(file);
    }
    if (snap_shader != snap_shader_) {
      REXLOG_INFO("Renut: Plume snapshot of the first resolve shader {:016X} samples", snap_shader);
    }
    snap_shader_ = snap_shader;
    resolve_log_ = access((root + "/plume-resolve-log").c_str(), F_OK) == 0;
    {
      std::vector<ConstOverride> overrides;
      if (FILE* file = std::fopen((root + "/plume-const").c_str(), "r")) {
        char stage[8];
        while (std::fscanf(file, "%7s", stage) == 1) {
          if (stage[0] == 'b') {
            unsigned int word = 0, set = 0, clear = 0;
            if (std::fscanf(file, "%u %x %x", &word, &set, &clear) != 3) break;
            if (word < 8) {
              ConstOverride o = {false, word, {}};
              o.bool_word = true;
              o.set = set;
              o.clear = clear;
              overrides.push_back(o);
            }
          } else {
            unsigned int reg;
            float v[4];
            if (std::fscanf(file, "%u %f %f %f %f", &reg, &v[0], &v[1], &v[2], &v[3]) != 5) break;
            if (reg < 256) overrides.push_back({stage[0] == 'p', reg, {v[0], v[1], v[2], v[3]}});
          }
        }
        std::fclose(file);
      }
      if (overrides.size() != const_overrides_.size()) {
        REXLOG_INFO("Renut: Plume constant overrides: {}", overrides.size());
      }
      const_overrides_ = std::move(overrides);
    }
    {
      unsigned int fix = 1;
      if (FILE* file = std::fopen((root + "/plume-refractfix").c_str(), "r")) {
        if (std::fscanf(file, "%u", &fix) != 1) fix = 1;
        std::fclose(file);
      }
      refraction_fix_ = fix != 0;
    }
    {
      unsigned int on = 0;
      float scale = 1.0f;
      if (FILE* file = std::fopen((root + "/plume-polyoffset").c_str(), "r")) {
        const int read = std::fscanf(file, "%u %f", &on, &scale);
        if (read < 1) on = 0;
        if (read < 2) scale = 1.0f;
        std::fclose(file);
      }
      if ((on != 0) != poly_offset_ || scale != poly_offset_scale_) {
        REXLOG_INFO("Renut: Plume polygon offset {} x{}", on ? "on" : "off", scale);
      }
      poly_offset_ = on != 0;
      poly_offset_scale_ = scale;
    }
    {
      unsigned int dedupe = 1;
      if (FILE* file = std::fopen((root + "/plume-dedupe").c_str(), "r")) {
        if (std::fscanf(file, "%u", &dedupe) != 1) dedupe = 1;
        std::fclose(file);
      }
      if ((dedupe != 0) != twin_dedupe_) REXLOG_INFO("Renut: Plume twin-draw dedupe {}", dedupe ? "on" : "off");
      twin_dedupe_ = dedupe != 0;
    }
    unsigned long long depth_dump_ps = 0;
    if (FILE* file = std::fopen((root + "/plume-depth-dump").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &depth_dump_ps) != 1) depth_dump_ps = 0;
      std::fclose(file);
    }
    if (depth_dump_ps != depth_dump_ps_ && !depth_dump_) {
      REXLOG_INFO("Renut: Plume depth dump of the first depth resolve shader {:016X} samples", depth_dump_ps);
    }
    if (!depth_dump_) depth_dump_ps_ = depth_dump_ps;
    uint32_t first = 0, end = 0;
    if (FILE* file = std::fopen((root + "/plume-drawskip").c_str(), "r")) {
      if (std::fscanf(file, "%u %u", &first, &end) != 2) first = end = 0;
      std::fclose(file);
    }
    if (first != drawskip_first_ || end != drawskip_end_) {
      REXLOG_INFO("Renut: Plume dropping draws {}..{} of every frame", first, end);
    }
    drawskip_first_ = first;
    drawskip_end_ = end;
    unsigned long long skip_shader = 0;
    if (FILE* file = std::fopen((root + "/plume-skip-shader").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &skip_shader) != 1) skip_shader = 0;
      std::fclose(file);
    }
    if (skip_shader != skip_shader_) REXLOG_INFO("Renut: Plume dropping shader {:016X}", skip_shader);
    skip_shader_ = skip_shader;
    {
      std::unordered_set<uint64_t> list;
      bool present = false;
      if (FILE* file = std::fopen((root + "/plume-skin-skip").c_str(), "r")) {
        present = true;
        unsigned long long hash;
        while (std::fscanf(file, "%llx", &hash) == 1) list.insert(hash);
        std::fclose(file);
      }
      std::lock_guard<std::mutex> lock(skinned_mutex_);
      if (list != skin_skip_) REXLOG_INFO("Renut: Plume dropping {} skinned vertex shaders", list.size());
      skin_skip_ = std::move(list);
      skin_log_ = present || access((root + "/plume-skin-log").c_str(), F_OK) == 0;
    }
    unsigned long long debug_shader = 0;
    uint32_t debug_mode = 7;
    if (FILE* file = std::fopen((root + "/plume-debug-shader").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &debug_shader) != 1) debug_shader = 0;
      if (std::fscanf(file, "%u", &debug_mode) != 1) debug_mode = 7;
      std::fclose(file);
    }
    debug_mode_ = debug_mode;
    unsigned int white_slots = 0;
    if (FILE* file = std::fopen((root + "/plume-white-slots").c_str(), "r")) {
      if (std::fscanf(file, "%x", &white_slots) != 1) white_slots = 0;
      std::fclose(file);
    }
    if (white_slots != white_slots_) REXLOG_INFO("Renut: Plume white slots {:08X}", white_slots);
    white_slots_ = white_slots;
    unsigned long long trace_ps = 0;
    if (FILE* file = std::fopen((root + "/plume-trace-ps").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &trace_ps) != 1) trace_ps = 0;
      std::fclose(file);
    }
    trace_ps_ = trace_ps;
    unsigned int pixel_center = 0;
    if (FILE* file = std::fopen((root + "/plume-pixel-center").c_str(), "r")) {
      if (std::fscanf(file, "%u", &pixel_center) != 1) pixel_center = 0;
      std::fclose(file);
    }
    if (pixel_center != pixel_center_debug_) REXLOG_INFO("Renut: Plume pixel centre mode {}", pixel_center);
    pixel_center_debug_ = pixel_center;
    if (debug_shader != debug_shader_) REXLOG_INFO("Renut: Plume debugging shader {:016X}", debug_shader);
    debug_shader_ = debug_shader;
  }
  // A range nothing has exported into for a while goes back to guest memory.
  // Emitters export every frame they are alive, so a couple of seconds is
  // plenty. At 600 frames a range outlived the level that exported into it:
  // the next level reused the memory for ordinary meshes, and every block it
  // wrote with the bytes the range's snapshot already held kept the old
  // level's exported vertices - triangles across the screen after a few
  // trips between Showdown Town and Nutty Acres.
  for (auto it = exports_.begin(); it != exports_.end();) {
    if (it->second.exported_frame + 30 < swaps_) {
      retired_buffers_[slot_].push_back(std::move(it->second.buffer));
      ++export_epoch_;
      it = exports_.erase(it);
    } else {
      ++it;
    }
  }
  if (swaps_ % 300 == 1) Report();
  if (swaps_ % 15 == 3) ReportMemory();
  BeginFrame();
}

void PlumeCommandProcessor::ReportMemory() {
  auto mb = [](uint64_t bytes) { return bytes >> 20; };
  // Base level plus the rest of the chain, in the 4-byte form every guest
  // texture is decoded to.
  auto chain = [](uint64_t base, uint32_t levels, uint32_t num, uint32_t den) {
    return levels > 1 ? base * num / den : base;
  };
  uint64_t tex2d = 0, tex2d_n = 0, tex2d_bc_n = 0, stacks = 0, volumes = 0, cubes = 0, resolved = 0, resolved_n = 0, target_bytes = 0;
  for (const auto& [key, t] : guest_textures_) {
    if (!t.texture) continue;
    tex2d += HostTextureBytes(t);
    ++tex2d_n;
    if (t.bc != BcKind::kNone) ++tex2d_bc_n;
  }
  for (const auto& [key, t] : guest_stacks_) {
    if (t.texture) stacks += chain(uint64_t(t.width) * t.height * t.depth * 4, t.levels, 4, 3);
  }
  for (const auto& [key, t] : guest_volumes_) {
    if (t.texture) volumes += chain(uint64_t(t.width) * t.height * t.depth * 4, t.levels, 8, 7);
  }
  for (const auto& [key, t] : guest_cubes_) {
    if (t.texture) cubes += chain(uint64_t(t.width) * t.height * 6 * 4, t.levels, 4, 3);
  }
  for (const auto& [key, r] : resolved_) {
    if (r.texture) {
      resolved += uint64_t(r.width) * r.height * RenderFormatSize(r.format);
      ++resolved_n;
    }
    if (r.fitted) resolved += uint64_t(r.width) * r.height * RenderFormatSize(r.format);
  }
  for (const auto& [key, t] : targets_) {
    if (t.texture) target_bytes += uint64_t(t.width) * t.height * RenderFormatSize(t.format);
  }
  uint64_t export_bytes = 0;
  for (const auto& [key, e] : exports_) export_bytes += e.size;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(static_cast<VulkanDevice*>(device_)->allocator, budgets);
  uint64_t blocks = 0, allocations = 0;
  for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) {
    blocks += budgets[i].statistics.blockBytes;
    allocations += budgets[i].statistics.allocationBytes;
  }
  const uint64_t previous = reported_memory_blocks_;
  if (blocks + (32ull << 20) > previous && blocks < previous + (32ull << 20)) return;
  reported_memory_blocks_ = blocks;
  uint64_t rss_kb = 0, swap_kb = 0;
  if (FILE* status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, status)) {
      std::sscanf(line, "VmRSS: %llu kB", reinterpret_cast<unsigned long long*>(&rss_kb));
      std::sscanf(line, "VmSwap: %llu kB", reinterpret_cast<unsigned long long*>(&swap_kb));
    }
    std::fclose(status);
  }
  REXLOG_INFO(
      "Renut: Plume memory @{}: vma blocks {} MB allocations {} MB | tex2d {} MB ({}, {} as BC) stacks {} MB "
      "volumes {} MB cubes {} MB resolved {} MB ({}) targets {} MB ({}) kept {} MB exports {} MB "
      "upload {} MB | rss {} MB swap {} MB",
      swaps_, mb(blocks), mb(allocations), mb(tex2d), tex2d_n, tex2d_bc_n, mb(stacks), mb(volumes), mb(cubes),
      mb(resolved), resolved_n, mb(target_bytes), targets_.size(), mb(kept_bytes_), mb(export_bytes),
      mb(upload_slice_size_ * kPlumeFramesInFlight), rss_kb >> 10, swap_kb >> 10);
}

void PlumeCommandProcessor::Report() {
  SyncRecorder();
  stats_.drawn += rec_stats_.drawn;
  stats_.pipeline_binds += rec_stats_.pipeline_binds;
  stats_.hiz_predicated_draws += rec_stats_.predicated;
  stats_.passes += rec_stats_.passes;
  rec_stats_ = {};
  REXLOG_INFO(
      "Renut: Plume frame {}: draws={} drawn={} no_vs={} no_ps={} bad_shader={} skipped_prim={} "
      "no_target={} resolves={} clears={} passes={} barriers={} pipeline_binds={} vertices={} gs_draws={} patch_draws={} patch_vertices={} adaptive={} split(color={} depth={} read={} src={} dst={}) pipelines={} targets={} resolved={} shaders={} "
      "missing_ucode={} upload_overflow={} upload_used={}KB textures={} decoded={} "
      "texture_failures={} frame_ms={} pipeline_ms={} submit_ms={} guest_swaps={} "
      "const_reused={} export(draws={} ranges={}/{} reads={} merges={} refreshes={} barriers={}) "
      "cp_cpu_ms={} slot_wait_ms={} stream_mb={} constant_mb={} kept(hits={} new_mb={} drops={} unwatchable={} total_mb={} streams={}) "
      "bands(skipped={} moved={} widened={})",
      swaps_, stats_.draws, stats_.drawn, stats_.no_vs, stats_.no_ps, stats_.bad_shader,
      stats_.skipped_prim, stats_.no_target, stats_.resolves, stats_.clears, stats_.passes, stats_.barriers,
      stats_.pipeline_binds, stats_.vertices, stats_.gs_draws, stats_.patch_draws, stats_.patch_vertices, stats_.adaptive_patch_draws,
      stats_.split_by[uint32_t(RenderTextureLayout::COLOR_WRITE) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::DEPTH_WRITE) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::SHADER_READ) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::COPY_SOURCE) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::COPY_DEST) & 7],
      stats_.pipelines,
      targets_.size(), resolved_.size(), shaders_.size(), missed_.size(), stats_.overflow,
      (upload_used_ - slice_base_) >> 10, guest_textures_.size(), stats_.textures_decoded,
      stats_.texture_failures, stats_.frame_us / 1000, stats_.pipeline_us / 1000,
      stats_.submit_us / 1000, renut_guest_swap_count() - reported_guest_swaps_,
      stats_.constants_reused, stats_.export_draws, exports_.size(), stats_.export_ranges,
      stats_.export_reads, stats_.export_merges, stats_.export_refreshes, stats_.export_barriers,
      stats_.cp_cpu_us / 1000, stats_.slot_wait_us / 1000, stats_.stream_bytes >> 20,
      stats_.constant_bytes >> 20, stats_.kept_hits, stats_.kept_bytes_made >> 20, stats_.kept_drops,
      stats_.kept_unwatchable, kept_bytes_ >> 20, kept_streams_.size(), stats_.band_skipped,
      stats_.band_moved, stats_.band_widened);
  REXLOG_INFO("Renut: Plume shader cache ({}): {} hits, {} entries, {} dropped by writes", shader_cache_ ? "on" : "off",
              shader_cache_hits_, shader_watch_.size(), shader_cache_drops_);
  if (stats_.defer_windows) {
    REXLOG_INFO("Renut: Plume windows closed by: elsewhere {} ({} draws), state change {} ({}), full {} ({}), "
                "immediate draw {} ({}), the draw's own needs {} ({})",
                stats_.flush_windows[0], stats_.flush_draws[0], stats_.flush_windows[1], stats_.flush_draws[1],
                stats_.flush_windows[2], stats_.flush_draws[2], stats_.flush_windows[3], stats_.flush_draws[3],
                stats_.flush_windows[4], stats_.flush_draws[4]);
    REXLOG_INFO("Renut: Plume deferred draws (mode {}): {} draws in {} windows, pipeline changes {} -> {}",
                defer_mode_, stats_.deferred_draws, stats_.defer_windows, stats_.defer_binds_before,
                stats_.defer_binds_after);
  }
  if (hiz_mode_ && (stats_.hiz_seen[0] || stats_.hiz_seen[1])) {
    REXLOG_INFO(
        "Renut: Plume Hi-Z (mode {}): colour seen={} boxed={} no_model={} no_box={} frustum={} scissor={} | "
        "depth-only seen={} boxed={} no_model={} no_box={} frustum={} scissor={} | dropped={} | models {}/{} valid "
        "({} rejected, {} ms), boxes {} made ({} ms, {} reused) | skipped: no_indices={} prim={} memexport={} clip_state={} "
        "const_base={}",
        hiz_mode_, stats_.hiz_seen[1], stats_.hiz_eligible[1], stats_.hiz_no_model[1], stats_.hiz_no_box[1],
        stats_.hiz_frustum[1], stats_.hiz_scissor[1], stats_.hiz_seen[0], stats_.hiz_eligible[0],
        stats_.hiz_no_model[0], stats_.hiz_no_box[0], stats_.hiz_frustum[0], stats_.hiz_scissor[0],
        stats_.hiz_culled_total, stats_.hiz_models_valid, stats_.hiz_models, stats_.hiz_model_rejected,
        stats_.hiz_model_us / 1000, stats_.hiz_boxes_made, stats_.hiz_box_us / 1000, stats_.hiz_box_hits,
        stats_.hiz_why[0], stats_.hiz_why[1], stats_.hiz_why[2], stats_.hiz_why[3], stats_.hiz_why[4]);
  }
  if (hiz_mode_ && !hiz_nomodel_draws_.empty()) {
    std::vector<std::pair<uint64_t, uint64_t>> ranked;
    for (const auto& [key, count] : hiz_nomodel_draws_) ranked.emplace_back(count, key);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string line = "Renut: Plume Hi-Z shaders without a model (vs hash/why/draws):";
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 12); ++i) {
      uint64_t hash = ranked[i].second;
      uint32_t why = 0;
      for (uint32_t w = 0; w < 16; ++w) {
        auto it = hiz_nomodel_why_.find(hash ^ w);
        if (it != hiz_nomodel_why_.end() && it->second == w) { why = w; hash ^= w; break; }
      }
      line += fmt::format(" [{:016X}/{}/{}]", hash, why, ranked[i].first);
    }
    REXLOG_INFO("{}", line);
    hiz_nomodel_draws_.clear();
  }
  if (hiz_mode_ && !hiz_clip_states_.empty()) {
    std::vector<std::pair<uint64_t, uint64_t>> ranked;
    for (const auto& [state, count] : hiz_clip_states_) ranked.emplace_back(count, state);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string line = "Renut: Plume Hi-Z skipped clip states (vte/clip):";
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 6); ++i) {
      line += fmt::format(" [{:08X}/{:08X} x{}]", uint32_t(ranked[i].second >> 32), uint32_t(ranked[i].second),
                          ranked[i].first);
    }
    REXLOG_INFO("{}", line);
    hiz_clip_states_.clear();
  }
  if (hiz_mode_ >= 3) {
    REXLOG_INFO(
        "Renut: Plume Hi-Z occlusion: {} windows tested ({} failed), {} draws tested, {} held back, of which {} "
        "answered hidden and {} visible; {} answered ({} hidden in all); {} draws run under conditional rendering; "
        "history {} keys | boxed draws {} (not deferred {}, biased {}, other depth func {}, no depth {}); windows {} "
        "(not tested {} holding {} draws); learning every {} frames; verified {} held draws: {} violations, {} "
        "visible answers with no samples ({} of them over depth still at the far plane)",
        stats_.hiz_windows_tested, stats_.hiz_test_failed, stats_.hiz_records, stats_.hiz_held,
        stats_.hiz_held_skipped, stats_.hiz_held_visible, stats_.hiz_answered, stats_.hiz_occluded,
        stats_.hiz_predicated_draws, hiz_history_.size(), stats_.hiz_boxed_draws, stats_.hiz_why_not[0],
        stats_.hiz_why_not[1], stats_.hiz_why_not[2], stats_.hiz_why_not[3], stats_.hiz_windows_all,
        stats_.hiz_small_windows, stats_.hiz_small_draws, hiz_learn_period_, stats_.hiz_verified,
        stats_.hiz_violations, stats_.hiz_missed_hidden, stats_.hiz_missed_empty);
  }
  if (!hiz_zero_pairs_.empty()) {
    std::vector<std::array<uint64_t, 3>> ranked;
    uint64_t total = 0;
    for (const auto& [key, entry] : hiz_zero_pairs_) {
      ranked.push_back(entry);
      total += entry[0];
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a[0] > b[0]; });
    std::string line = fmt::format("Renut: Plume Hi-Z draws with no samples that the test could not call hidden ({}):", total);
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 8); ++i) {
      line += fmt::format(" [vs {:016X} ps {:016X} x{}]", ranked[i][1], ranked[i][2], ranked[i][0]);
    }
    REXLOG_INFO("{}", line);
    hiz_zero_pairs_.clear();
  }
  if (bc_supported_ && bc_mode_) {
    REXLOG_INFO("Renut: Plume BC textures: {} of the {} 2D uploads were blocks, not decoded pixels",
                stats_.textures_bc, stats_.textures_decoded);
  }
  if (shadow_min_indices_) {
    REXLOG_INFO("Renut: Plume shadow casters under {} indices dropped: {}", shadow_min_indices_,
                stats_.shadow_culled);
  }
  if (depth_alias_ || stats_.depth_aliased) {
    REXLOG_INFO("Renut: Plume depth maps in place ({}): {} resolves without a copy, {} copied back, {} dropped by a clear",
                depth_alias_ ? "on" : "off", stats_.depth_aliased, stats_.depth_restored, stats_.depth_dropped);
  }
  // Keep what the driver compiled this interval, so the next run of this scene
  // does not spend its frames inside vkCreateGraphicsPipelines. Android rarely
  // shuts the process down cleanly, so waiting for the device's own save would
  // usually lose the blob.
  if (gpu_ns_) {
    std::vector<std::pair<uint64_t, uint64_t>> ranked;  // ns, key
    for (const auto& [key, total] : pass_totals_) ranked.emplace_back(total.ns, key);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string line = fmt::format("Renut: Plume GPU {} ms over {} swaps, by pass:",
                                   gpu_ns_ / 1000000, swaps_ - reported_guest_swaps_);
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 8); ++i) {
      const PassTotal& total = pass_totals_[ranked[i].second];
      const uint64_t key = ranked[i].second;
      line += fmt::format(" [{}{}x{} c{}{} {}ms {}% {}passes {}draws]",
                          (key >> 62) == 1 ? "entre-passes " : "",
                          uint32_t((key >> 32) & 0x3FFFFFFF),
                          uint32_t((key >> 8) & 0xFFFFFF), uint32_t((key >> 1) & 0x7F),
                          (key & 1) ? "+d" : "", total.ns / 1000000, 100 * total.ns / gpu_ns_,
                          total.passes, total.draws);
    }
    REXLOG_INFO("{}", line);
    pass_totals_.clear();
    gpu_ns_ = 0;
  }
  if (stats_.pipelines || stats_.lookups_missed) {
    uint64_t found = 0, compiled = 0;
    PlumePipelineCacheCounts(&found, &compiled);
    REXLOG_INFO("Renut: Plume new pipelines: {} built ahead, {} found in the cache at the draw ({} us per lookup, "
                "{} missed), {} found by a worker, {} compiled ({} ms each, {} after waiting for a sibling's "
                "compile); since launch the driver's cache had {} and compiled {}",
                stats_.pipelines_prebuilt, stats_.pipelines_found_here,
                stats_.lookup_us / std::max<uint64_t>(1, stats_.pipelines_found_here + stats_.lookups_missed),
                stats_.lookups_missed, stats_.pipelines_found_ahead, stats_.pipelines_compiled,
                stats_.compile_us / 1000 / std::max<uint64_t>(1, stats_.pipelines_compiled),
                stats_.pipelines_waited, found, compiled);
  }
  SavePlumePipelineCache();
  // A gap in the sound is a frame the output had to fill with silence; each is
  // 256 samples, 5.3 ms at 48 kHz.
  const uint64_t silent = renut_audio_silent_frames();
  if (silent != reported_silent_frames_) {
    REXLOG_INFO("Renut: audio played {} frames of silence ({} ms) since the last report",
                silent - reported_silent_frames_, (silent - reported_silent_frames_) * 16 / 3);
    reported_silent_frames_ = silent;
  }
  for (const auto& [hash, count] : missed_draws_) {
    REXLOG_INFO("Renut: Plume   missing VS {:016X}: {} draws", hash, count);
  }
  missed_draws_.clear();
  if (!drops_.empty()) {
    std::map<std::string, uint64_t> by_reason;
    std::vector<std::pair<uint64_t, std::tuple<std::string, uint64_t, uint64_t, bool>>> ranked;
    for (const auto& [key, count] : drops_) {
      by_reason[std::get<0>(key)] += count;
      ranked.emplace_back(count, key);
    }
    std::string line = "Renut: Plume drops:";
    for (const auto& [reason, count] : by_reason) line += fmt::format(" [{}] {}", reason, count);
    REXLOG_INFO("{}", line);
    std::sort(ranked.rbegin(), ranked.rend());
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 12); ++i) {
      const auto& [count, key] = ranked[i];
      REXLOG_INFO("Renut: Plume   drop [{}] vs={:016X} ps={:016X}{}: {}", std::get<0>(key), std::get<1>(key),
                  std::get<2>(key), std::get<3>(key) ? " memexport" : "", count);
    }
    drops_.clear();
  }
  // One line a report in <files>/plume-hiz.txt whatever the log level is, for
  // A/B runs that cannot read the log.
  if (!miss_dump_directory_.empty()) {
    const std::string path = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/plume-hiz.txt";
    if (FILE* out = std::fopen(path.c_str(), "a")) {
      std::fprintf(out,
                   "mode=%u swaps=%llu draws=%llu drawn=%llu dropped=%llu color_seen=%llu color_boxed=%llu "
                   "color_frustum=%llu depth_seen=%llu depth_boxed=%llu depth_frustum=%llu windows_tested=%llu "
                   "tested=%llu held=%llu answered=%llu hidden=%llu predicated=%llu test_failed=%llu cp_cpu_ms=%llu "
                   "cull_ms=%llu model_ms=%llu box_ms=%llu eval_ms=%llu memo_hits=%llu pipeline_binds=%llu nm_color=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu nm_depth=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu skipped=%llu,%llu skip=%d tests_ms=%llu\n",
                   hiz_mode_, (unsigned long long)swaps_, (unsigned long long)stats_.draws,
                   (unsigned long long)stats_.drawn, (unsigned long long)stats_.hiz_culled_total,
                   (unsigned long long)stats_.hiz_seen[1], (unsigned long long)stats_.hiz_eligible[1],
                   (unsigned long long)stats_.hiz_frustum[1], (unsigned long long)stats_.hiz_seen[0],
                   (unsigned long long)stats_.hiz_eligible[0], (unsigned long long)stats_.hiz_frustum[0],
                   (unsigned long long)stats_.hiz_windows_tested, (unsigned long long)stats_.hiz_records,
                   (unsigned long long)stats_.hiz_held, (unsigned long long)stats_.hiz_answered,
                   (unsigned long long)stats_.hiz_occluded, (unsigned long long)stats_.hiz_predicated_draws,
                   (unsigned long long)stats_.hiz_test_failed, (unsigned long long)(stats_.cp_cpu_us / 1000),
                   (unsigned long long)(stats_.hiz_cull_ns / 1000000), (unsigned long long)(stats_.hiz_stage_ns[0] / 1000000),
                   (unsigned long long)(stats_.hiz_stage_ns[1] / 1000000), (unsigned long long)(stats_.hiz_stage_ns[2] / 1000000),
                   (unsigned long long)stats_.stream_memo_hits, (unsigned long long)stats_.pipeline_binds,
                   (unsigned long long)stats_.hiz_why_model[1][1], (unsigned long long)stats_.hiz_why_model[1][2], (unsigned long long)stats_.hiz_why_model[1][3], (unsigned long long)stats_.hiz_why_model[1][4], (unsigned long long)stats_.hiz_why_model[1][5], (unsigned long long)stats_.hiz_why_model[1][6], (unsigned long long)stats_.hiz_why_model[1][7], (unsigned long long)stats_.hiz_why_model[1][8], (unsigned long long)stats_.hiz_why_model[1][9], (unsigned long long)stats_.hiz_why_model[1][10], (unsigned long long)stats_.hiz_why_model[0][1], (unsigned long long)stats_.hiz_why_model[0][2], (unsigned long long)stats_.hiz_why_model[0][3], (unsigned long long)stats_.hiz_why_model[0][4], (unsigned long long)stats_.hiz_why_model[0][5], (unsigned long long)stats_.hiz_why_model[0][6], (unsigned long long)stats_.hiz_why_model[0][7], (unsigned long long)stats_.hiz_why_model[0][8], (unsigned long long)stats_.hiz_why_model[0][9], (unsigned long long)stats_.hiz_why_model[0][10],
                   (unsigned long long)stats_.hiz_skipped[1], (unsigned long long)stats_.hiz_skipped[0], hiz_skip_ ? 1 : 0,
                   (unsigned long long)(stats_.hiz_stage_ns[3] / 1000000));
      std::fclose(out);
    }
  }
  reported_guest_swaps_ = renut_guest_swap_count();
  stats_ = {};
}

// ---------------------------------------------------------------------------
// Graphics system
// ---------------------------------------------------------------------------

// The GPU the app hosts: the guest's video hardware as the game's Direct3D and
// the kernel see it - the register window at 0x7FC80000, the vertical blank and
// its interrupt, the ring and the command processor behind it. The SDK has no
// part in it (RuntimeConfig::graphics is null); the three Vd exports that
// reach it are overridden in kernel_overrides.cpp.
class PlumeGpu {
 public:
  bool Setup(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel) {
    memory_ = dispatcher->memory();
    function_dispatcher_ = dispatcher;
    kernel_state_ = kernel;
    processor_ = std::make_unique<PlumeCommandProcessor>(
        memory_, kernel_state_, [this](uint32_t source, uint32_t cpu) { DispatchInterruptCallback(source, cpu); });
    if (!processor_->Initialize()) return false;
    // The GPU's registers are 0x7FC80000-0x7FCFFFFF; the game reads the display
    // counters there and writes the ring's write pointer.
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                   reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
                                   reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));
    vsync_running_ = true;
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return VblankClock(); }));
    vsync_thread_->set_name("GPU VSync");
    vsync_thread_->Create();
    return true;
  }

  void Shutdown() {
    if (processor_) {
      processor_->Shutdown();
      processor_.reset();
    }
    if (vsync_thread_) {
      vsync_running_ = false;
      vsync_thread_->Wait(0, 0, 0, nullptr);
      vsync_thread_.reset();
    }
  }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    interrupt_callback_ = callback;
    interrupt_callback_data_ = user_data;
    REXLOG_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    if (processor_) processor_->InitializeRingBuffer(ptr, size_log2);
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    if (processor_) processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
  }

 private:
  // Runs the guest's interrupt callback on the calling thread, as the console's
  // GPU interrupt would on the CPU it names.
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    if (cpu == 0xFFFFFFFF) cpu = 2;
    thread->SetActiveCpu(cpu);
    uint64_t args[] = {source, interrupt_callback_data_};
    function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                           rex::countof(args));
  }

  void MarkVblank() {
    // The counter the game sees move, then the interrupt that runs its handler.
    if (processor_) processor_->increment_counter();
    DispatchInterruptCallback(0, 2);
  }

  // The guest's vertical blank: one MarkVblank per refresh of the video mode the
  // game set (60 Hz), which is what its presentation interval counts and so what
  // holds it at 30 FPS.
  //
  // The SDK's worker polled every millisecond and, when it found itself late,
  // marked every vblank it owed in a row: a wake-up that overslept (common on a
  // phone's timer slack and under load) came out as two vblanks back to back and
  // then a gap, which the game showed as alternating short and long frames. This
  // one sleeps to an absolute deadline, spins the last stretch, marks exactly one
  // vblank per deadline, and when it fell more than two periods behind (the
  // process was descheduled, the interrupt ran long) it forgets the debt rather
  // than paying it as a burst. The deadlines are in the guest's clock: while the
  // game is paused its time crawls, and so do the vblanks.
  int VblankClock() {
    using pacing::Clock;
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    auto next = Clock::now();
    bool first = true;
    while (vsync_running_) {
      // Host time per vblank: a guest period stretched by the guest clock's scale.
      const double scale = std::max(0.001, rex::chrono::Clock::guest_time_scalar());
      const double seconds = REXCVAR_GET(vsync) ? 1.0 / hz : 0.001;
      const auto period = std::chrono::nanoseconds(int64_t(seconds / scale * 1e9));
      if (first) {
        next += period;
        first = false;
      }
      // Waits in slices so that a long period (paused) still notices the stop.
      for (;;) {
        if (!vsync_running_) return 0;
        const auto now = Clock::now();
        if (now >= next) break;
        const auto remaining = next - now;
        if (remaining > pacing::kSpinTail * 2) {
          std::this_thread::sleep_for(
              std::min<Clock::duration>(remaining - pacing::kSpinTail, std::chrono::milliseconds(4)));
        } else {
          std::this_thread::yield();
        }
      }
      MarkVblank();
      next = pacing::NextTick(next, Clock::now(), period);
    }
    return 0;
  }

  static uint32_t ReadRegisterThunk(void*, PlumeGpu* gpu, uint32_t addr) { return gpu->ReadRegister(addr); }
  static void WriteRegisterThunk(void*, PlumeGpu* gpu, uint32_t addr, uint32_t value) {
    gpu->WriteRegister(addr, value);
  }

  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
      case 0x0F00:  // RB_EDRAM_TIMING
        return 0x08100748;
      case 0x0F01:  // RB_BC_CONTROL
        return 0x0000200E;
      case 0x194C: {  // R500_D1MODE_V_COUNTER
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case 0x1951:  // interrupt status
        return 1;   // vblank
      case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE: [width(0x0FFF), height(0x0FFF)]
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default:
        break;
    }
    return processor_ && r < gfx::RegisterFile::kRegisterCount ? processor_->ReadRegisterValue(r) : 0;
  }

  void WriteRegister(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r == 0x01C5) {  // CP_RB_WPTR
      if (processor_) processor_->UpdateWritePointer(value);
      return;
    }
    if (r == 0x1844) return;  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
    if (processor_ && r < gfx::RegisterFile::kRegisterCount) processor_->registers()->values[r] = value;
  }

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  std::unique_ptr<PlumeCommandProcessor> processor_;
  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  std::atomic<bool> vsync_running_{false};
  rex::system::object_ref<rex::system::XHostThread> vsync_thread_;
};

}  // namespace

// The GPU hosted by the app (plume_gpu.h).
namespace {
std::unique_ptr<PlumeGpu> g_hosted_gpu;
}  // namespace

bool StartPlumeGpu(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel) {
  if (g_hosted_gpu) return true;
  auto gpu = std::make_unique<PlumeGpu>();
  if (!gpu->Setup(dispatcher, kernel)) {
    gpu->Shutdown();
    return false;
  }
  g_hosted_gpu = std::move(gpu);
  return true;
}

void StopPlumeGpu() {
  if (!g_hosted_gpu) return;
  g_hosted_gpu->Shutdown();
  g_hosted_gpu.reset();
}

bool PlumeGpuHosted() { return g_hosted_gpu != nullptr; }

void PlumeGpuSetInterruptCallback(uint32_t callback, uint32_t user_data) {
  if (g_hosted_gpu) g_hosted_gpu->SetInterruptCallback(callback, user_data);
}

void PlumeGpuInitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  if (g_hosted_gpu) g_hosted_gpu->InitializeRingBuffer(ptr, size_log2);
}

void PlumeGpuEnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  if (g_hosted_gpu) g_hosted_gpu->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void SetPlumeMissDumpDirectory(const std::string& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  g_miss_dump_directory = directory;
}

void ArmPlumeMemoryTrace(const std::string& root) {
  g_dedupe_descriptors = access((root + "/plume-dedupe-desc").c_str(), F_OK) == 0;
  g_validate_windows = access((root + "/plume-validate").c_str(), F_OK) == 0;
}

void PlumeSceneRequested(uint32_t scene, uint32_t asset, uint32_t ref) {
  PrecompileState& p = g_precompile;
  p.scene.store(scene, std::memory_order_relaxed);
  std::lock_guard lock(p.mutex);
  // The scene's groups that are not built yet go to the front of the queue.
  p.scene_missing.clear();
  p.scene_total = p.scene_left = 0;
  const auto groups = p.scene_groups.find(scene);
  if (groups != p.scene_groups.end()) {
    for (const uint32_t g : groups->second) {
      p.scene_total += p.group_size[g];
      if (p.state[g] == PrecompileState::kDone) continue;
      p.scene_left += p.group_size[g];
      p.scene_missing.push_back(g);
      if (p.state[g] == PrecompileState::kPending) p.urgent.push_front(g);
    }
  }
  p.scene_asked = p.scene_moved = std::chrono::steady_clock::now();
  if (!p.running) p.scene_missing.clear();
  p.loading = !p.scene_missing.empty();
  if (groups == p.scene_groups.end()) {
    REXLOG_INFO("Renut: scene {:08X} (asset {:08X}, ref {:08X}) requested; none of its pipelines known yet", scene,
                asset, ref);
  } else {
    REXLOG_INFO("Renut: scene {:08X} (asset {:08X}, ref {:08X}) requested; {} of its {} pipelines left to build", scene,
                asset, ref, p.scene_missing.empty() ? 0 : p.scene_left, p.scene_total);
  }
}

bool PlumeSceneReady() {
  PrecompileState& p = g_precompile;
  std::lock_guard lock(p.mutex);
  if (p.scene_missing.empty()) return true;
  const size_t before = p.scene_missing.size();
  std::erase_if(p.scene_missing, [&](uint32_t g) {
    if (p.state[g] != PrecompileState::kDone) return false;
    p.scene_left -= p.group_size[g];
    return true;
  });
  const auto now = std::chrono::steady_clock::now();
  if (p.scene_missing.size() != before) p.scene_moved = now;
  // Gives up only if the building stops moving: a stuck driver must not hold
  // the game for ever.
  if (!p.scene_missing.empty() && p.running && now - p.scene_moved < std::chrono::seconds(60)) return false;
  if (!p.scene_missing.empty() && p.running) {
    REXLOG_WARN("Renut: scene {:08X}: its pipelines stopped with {} of {} left; it opens without them",
                p.scene.load(std::memory_order_relaxed), p.scene_left, p.scene_total);
  }
  p.scene_missing.clear();
  return true;
}

void PlumeSceneOpened(bool held) {
  PrecompileState& p = g_precompile;
  std::lock_guard lock(p.mutex);
  p.loading = false;
  const auto now = std::chrono::steady_clock::now();
  REXLOG_INFO("Renut: scene {:08X} opens {} ms after its request{}; {} of its pipelines were known",
              p.scene.load(std::memory_order_relaxed),
              std::chrono::duration_cast<std::chrono::milliseconds>(now - p.scene_asked).count(),
              held ? ", its loading screen held for the pipelines" : "", p.scene_total);
}

}  // namespace renut
