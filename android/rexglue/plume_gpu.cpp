#include "plume_gpu.h"

#include "plume_renderer.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "plume_textures.h"
#include "plume_vulkan.h"
#include "shader_cache.h"
#include "xenos_ucode_key.h"
#include "smolv.h"
#include "spirv_depth_only.h"
#include "spirv_fetch_spec.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/memory.h>

#include <fmt/format.h>

#include <cstdio>
#include <filesystem>
#include <string>

void renutReportShadowEngineProfile();

REXCVAR_DEFINE_BOOL(renut_plume_async_pipelines, false, "reNut/Plume",
                    "Create new Plume graphics pipelines on a worker; non-export draws are skipped until ready.");
REXCVAR_DEFINE_INT32(renut_plume_defer_draws, 1, "reNut/Plume",
                     "Record draws in windows grouped by pipeline: 0 off, 1 depth-only draws (the shadow "
                     "maps), 2 also opaque colour draws that test and write depth (reordering them is "
                     "exact except for depth ties)")
    .range(0, 2);
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
REXCVAR_DEFINE_BOOL(renut_plume_memexport_readback, false, "reNut/Plume",
                    "Copy shader exports back to guest memory (experimental; may overwrite reused memory).");
REXCVAR_DEFINE_BOOL(renut_plume_memexport_fast, true, "reNut/Plume",
                    "Batch Plume memexport readback across frames; off waits for the current frame.");

// game_fixes.cpp: how the MSAA-off path treats the scene's band split
// (<files>/plume-keep-split: 0 clear, 1 leave, 2 whole-screen projection).
extern std::atomic<int> g_renut_scene_split_mode;
REXCVAR_DECLARE(bool, renut_scene_msaa);

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
#include "shaders/debug_point.frag.inc"
#include "shaders/debug_trivial.frag.inc"
#include "shaders/debug_trivial.vert.inc"
#include "shaders/rect_list.geom.inc"

// Where microcode without a recompiled shader is saved (SetPlumeMissDumpDirectory).
std::string g_miss_dump_directory;

// The game's own VdSwap count, which is what the on-screen counter shows. Plume
// presents more often than the game swaps, so the two rates have to be read
// side by side to know which one the player is actually seeing.
extern "C" uint64_t renut_guest_swap_count();

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
constexpr uint64_t kD3DClearVertexShader = 0xBA454DD8A2FCFCBAull;

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
  // constants instead of the uniform block (spirv_fetch_spec.h). Only vertex
  // and domain shaders with live fetches have one.
  std::unique_ptr<RenderShader> fetch_spec_module;
  // The vertex shader's two forms again without its interpolant outputs
  // (spirv_depth_only.h), for draws that have no fragment shader.
  std::unique_ptr<RenderShader> depth_module;
  std::unique_ptr<RenderShader> depth_fetch_spec_module;
  std::vector<std::pair<uint32_t, bool>> inputs;  // vertex shaders only
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
    const uint8_t* data = g_spirvCache + entry->spirvOffset;
    size_t size = smolv::GetDecodedBufferSize(data, entry->spirvSize);
    std::vector<uint32_t> spirv((size + 3) / 4);
    if (!size || !smolv::Decode(data, entry->spirvSize, spirv.data(), size)) {
      compiled.failed = true;
      return nullptr;
    }
    compiled.module = device->createShader(spirv.data(), size, "main", RenderShaderFormat::SPIRV);
    if (!compiled.module) {
      compiled.failed = true;
      return nullptr;
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
    return &compiled;
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
RenderFormat VertexFormat(uint32_t format, bool is_signed, bool normalized, InputDecode& decode) {
  decode = InputDecode::kNone;
  auto integer = [&](RenderFormat unsigned_format, RenderFormat signed_format) {
    decode = InputDecode::kInteger;
    return is_signed ? signed_format : unsigned_format;
  };
  switch (format) {
    case 6:
      return normalized ? (is_signed ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM)
                        : integer(RenderFormat::R8G8B8A8_UINT, RenderFormat::R8G8B8A8_SINT);
    case 25:
      return normalized ? (is_signed ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM)
                        : integer(RenderFormat::R16G16_UINT, RenderFormat::R16G16_SINT);
    case 26:
      return normalized
                 ? (is_signed ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM)
                 : integer(RenderFormat::R16G16B16A16_UINT, RenderFormat::R16G16B16A16_SINT);
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
        return normalized ? RenderFormat::R10G10B10A2_UNORM
                          : integer(RenderFormat::R10G10B10A2_UINT, RenderFormat::R10G10B10A2_UINT);
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
  // Serial of the last draw that rendered to it (see IssueD3DClear).
  uint64_t last_used = 0;
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
};

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
  uint64_t textures_decoded = 0, texture_failures = 0;
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
};

class PlumeCommandProcessor final : public gfx::CommandProcessor {
 public:
  PlumeCommandProcessor(gfx::GraphicsSystem* graphics_system, rex::system::KernelState* kernel)
      : CommandProcessor(graphics_system, kernel) {}

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  gfx::Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                          const uint32_t* host_address, uint32_t dword_count) override;
  // Register writes. The shader constants (ALU, fetch, boolean and loop,
  // 0x4000-0x4927) have no side effects in the base class, and the guest
  // writes thousands of them a frame through one virtual call each: those
  // ranges are swapped in bulk here, and every write into a stage's float
  // constants marks that stage's block as needing a look (CommitConstants).
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
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

  static constexpr uint32_t kConstantChunks = 4;
  static constexpr uint32_t kConstantChunkSize = 1024;
  bool constants_written_[2][kConstantChunks] = {{true, true, true, true}, {true, true, true, true}};
  // A draw left out for a reason no other counter names: counted by reason
  // and shader for the periodic report, and named in a trace.
  bool Drop(const char* reason, const HostShader* vs, const HostShader* ps);
  // Debug: a frame trace lists the draws the bin check drops, and
  // <files>/plume-pred-all "1" runs every predicated packet.
  bool OnPredicatedSkip(uint32_t opcode) override {
    if (trace_ && (opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2)) {
      std::fprintf(trace_, "predicated %s op=%X select=%llX mask=%llX\n", pred_pass_all_ ? "passed" : "skip",
                   opcode, (unsigned long long)bin_select_, (unsigned long long)bin_mask_);
    }
    return pred_pass_all_;
  }
  bool pred_pass_all_ = false;
  bool band_fragpos_ = false;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  // Direct3D's own Clear (see IssueDraw).
  void IssueD3DClear(xenos::EdramMode edram_mode);
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

 private:
  // Upload ring (one frame; reset after every swap).
  uint64_t Allocate(uint64_t size, uint64_t alignment = 16);
  uint8_t* Pointer(uint64_t offset) { return upload_data_ + offset; }
  uint64_t Address(uint64_t offset) const { return upload_address_ + offset; }
  bool UploadGuest(uint32_t address, uint32_t size, xenos::Endian endian, uint64_t& offset);
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
    uint64_t last_used = 0, dropped_frame = 0;
    uint32_t drops = 0;
    bool valid = false;
  };
  std::unordered_map<uint64_t, KeptStream> kept_streams_;
  LookupMemo<KeptStream, 13> kept_memo_;
  uint64_t kept_bytes_ = 0;
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
  Target* GetTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch, uint32_t height);
  Target* FindTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch = 0);
  RenderFramebuffer* GetFramebuffer(const std::array<Target*, 4>& colors, uint32_t color_count,
                                    Target* depth);
  void SetFramebuffer(RenderFramebuffer* framebuffer);
  Resolved* FindResolvedContaining(uint32_t address, RenderFormat format, uint32_t bytes_per_pixel,
                                  uint32_t pitch, uint32_t height,
                                  uint32_t& column, uint32_t& row);
  uint32_t FittedResolved(Resolved& resolved, uint32_t width, uint32_t height);
  void MergeResolvedTails(uint32_t address, Resolved& dest);
  Resolved& GetResolved(uint32_t address, RenderFormat format, uint32_t width, uint32_t height);
  // Everything LayoutOf works out from a fetch constant, kept per constant.
  struct FetchLayout {
    uint64_t key = 0;
    uint32_t levels = 0, top_level = 0;
    std::array<TextureLevel, 14> levels_layout{};
    std::array<uint32_t, 14> starts{};
    bool usable = false;
  };
  const FetchLayout* LayoutOf(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestCubeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestStackedTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestVolumeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch);
  void Report();

  RenderDevice* device_ = nullptr;
  RenderCommandQueue* queue_ = nullptr;
  std::unique_ptr<ShaderStore> store_;
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> shaders_;
  // LoadShader's answers this frame, by microcode address: {dwords, shader}.
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
  bool merge_bands_ = false;
  bool merge_frame_ = false;  // merging this frame
  bool band2_ = false;        // the lower band's replay has begun this frame
  uint32_t band_color_base_ = UINT32_MAX, band_depth_base_ = UINT32_MAX, band_pitch_ = 0;
  uint32_t band_h_ = 0, band_full_h_ = 0;
  uint64_t band_seen_frame_ = 0;
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
  uint32_t max_draw_height_ = 0;
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
  std::unique_ptr<RenderShader> debug_point_fs_;  // plume-debug-shader mode 8
  // Debug: <files>/plume-trivial holds 1 to swap every draw's shaders for
  // debug_trivial.{vert,frag} (nothing is rasterised), 2 only its pixel
  // shader. What the GPU still spends is what a draw costs apart from the
  // guest's shaders - the split the per-draw cost has been missing.
  std::unique_ptr<RenderShader> debug_trivial_vs_, debug_trivial_fs_;
  uint32_t trivial_mode_ = 0;

  // Keyed by guest base. Never overlapping: a new range swallows the ones it touches.
  std::map<uint32_t, ExportRange> exports_;
  std::array<std::vector<std::unique_ptr<RenderBuffer>>, kPlumeFramesInFlight> retired_buffers_;

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
    enum class Kind : uint8_t { kNone, kIndexed, kPatch, kPlain };
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
    uint32_t constant_offsets[kConstantBindings] = {};
    RenderViewport viewport;
    RenderRect scissor;
    uint32_t first_slot = 0, slot_count = 0;
    Kind kind = Kind::kNone;
    RenderBuffer* index_buffer = nullptr;
    RenderFormat index_format = RenderFormat::R16_UINT;
    uint32_t count = 0, first = 0;
    int32_t base_vertex = 0;
    // Only for the pair profile.
    uint64_t vs_hash = 0, ps_hash = 0;
  };
  bool PairBegin(const DrawPacket& pk);
  static constexpr size_t kMaxDeferred = 384;
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
  void FlushDeferred();
  void EmitDraw(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count);
  // The command list, after recording any draws that are waiting.
  RenderCommandList* L() {
    if (!deferred_.empty()) FlushDeferred();
    return list_;
  }

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
  };
  std::mutex pipeline_mutex_;
  std::condition_variable pipeline_ready_;
  std::deque<std::shared_ptr<PipelineJob>> pipeline_queue_;
  std::unordered_map<uint64_t, std::shared_ptr<PipelineJob>> pending_pipelines_;
  std::unordered_set<uint64_t> failed_pipelines_;
  std::thread pipeline_worker_;
  bool stop_pipeline_worker_ = false;
  void StartPipelineWorker();
  void StopPipelineWorker();
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
    if (!free_descriptors_.empty()) {
      uint32_t index = free_descriptors_.back();
      free_descriptors_.pop_back();
      return index;
    }
    return next_descriptor_ < kMaxTextures2D ? next_descriptor_++ : 0;
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
      resolved_lookup_.clear();
      resolved_lookup_frame_ = swaps_;
    }
    auto known = resolved_lookup_.find(address);
    if (known != resolved_lookup_.end()) return known->second;
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
    resolved_lookup_.emplace(address, latest);
    return latest;
  }
  void InvalidateResolvedLookups() { resolved_lookup_.clear(); }
  std::unordered_map<uint32_t, Resolved*> resolved_lookup_;
  uint64_t resolved_lookup_frame_ = UINT64_MAX;
  uint64_t resolve_serial_ = 0;
  uint64_t target_serial_ = 0;
  std::unordered_map<uint64_t, GuestTexture> guest_textures_;
  LookupMemo<GuestTexture, 12> texture_memo_;
  std::unordered_set<uint32_t> unsupported_formats_;
  uint32_t next_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_cubes_;
  uint32_t next_cube_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_stacks_;
  std::unordered_map<uint64_t, GuestTexture> guest_volumes_;
  uint32_t next_3d_descriptor_ = 1;

  Stats stats_;
  uint64_t swaps_ = 0;
};

void PlumeCommandProcessor::StartPipelineWorker() {
  if (pipeline_worker_.joinable()) return;
  stop_pipeline_worker_ = false;
  pipeline_worker_ = std::thread([this] {
    for (;;) {
      std::shared_ptr<PipelineJob> job;
      {
        std::unique_lock lock(pipeline_mutex_);
        pipeline_ready_.wait(lock, [this] { return stop_pipeline_worker_ || !pipeline_queue_.empty(); });
        if (stop_pipeline_worker_) return;
        job = std::move(pipeline_queue_.front());
        pipeline_queue_.pop_front();
      }
      job->desc.inputElements = job->elements.data();
      job->desc.inputSlots = job->slots.data();
      job->desc.specConstants = job->constants.data();
      const auto start = std::chrono::steady_clock::now();
      try { job->result = device_->createGraphicsPipeline(job->desc); }
      catch (const std::exception& e) { REXLOG_WARN("Renut: Plume async pipeline failed: {}", e.what()); }
      job->duration_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count());
      job->done.store(true, std::memory_order_release);
    }
  });
}

void PlumeCommandProcessor::StopPipelineWorker() {
  if (!pipeline_worker_.joinable()) return;
  {
    std::lock_guard lock(pipeline_mutex_);
    stop_pipeline_worker_ = true;
    pipeline_queue_.clear();
  }
  pipeline_ready_.notify_one();
  pipeline_worker_.join();
  pending_pipelines_.clear();
}

bool PlumeCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) return false;
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
  RenderPushConstantRange push(0, 0, 0, 3 * sizeof(uint64_t),
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

  // One slice per frame in flight, after the permanent allocations.
  const uint64_t upload_bytes = kUploadSize * kPlumeFramesInFlight + 4096;
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
  fetch_spec_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_fetch_spec), 0, 1));
  depth_strip_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_depth_strip), 0, 1));
  rel_constants_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_rel_constants), 0, 2));
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
  debug_point_fs_ = device_->createShader(k_debug_point_frag, sizeof(k_debug_point_frag), "main",
                                          RenderShaderFormat::SPIRV);
  debug_trivial_vs_ = device_->createShader(k_debug_trivial_vert, sizeof(k_debug_trivial_vert), "main",
                                            RenderShaderFormat::SPIRV);
  debug_trivial_fs_ = device_->createShader(k_debug_trivial_frag, sizeof(k_debug_trivial_frag), "main",
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
  writeback_ = REXCVAR_GET(renut_plume_memexport_readback);
  writeback_fast_ = REXCVAR_GET(renut_plume_memexport_fast);
  BeginFrame();
  REXLOG_INFO("Renut: Plume command processor ready ({} frames in flight, upload {} MB at {:X})",
              kPlumeFramesInFlight, upload_bytes >> 20, upload_address_);
  return true;
}

void PlumeCommandProcessor::ShutdownContext() {
  StopPipelineWorker();
  if (list_open_) {
    L()->end();
    list_open_ = false;
  }
  // Frames may still be running; nothing below may be freed under them.
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForAllSlots();
  pipelines_.clear();
  pipeline_memo_.Clear();
  framebuffers_.clear();
  targets_.clear();
  resolved_.clear();
  InvalidateResolvedLookups();
  exports_.clear();
  patch_grids_.clear();
  if (watch_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(watch_handle_);
    watch_handle_ = nullptr;
  }
  kept_streams_.clear();
  kept_memo_.Clear();
  kept_bytes_ = 0;
  for (auto& retired : retired_buffers_) retired.clear();
  for (auto& buffer : writeback_buffers_) buffer.reset();
  for (auto& list : writebacks_) list.clear();
  shader_lookup_.clear();
  shader_memo_.Clear();
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
  CommandProcessor::ShutdownContext();
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
  const uint64_t limit = slice_base_ + kUploadSize;
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

void PlumeCommandProcessor::DrainWatchHits() {
  const uint32_t serial = watch_serial_.load(std::memory_order_acquire);
  if (serial == watch_seen_) return;
  watch_seen_ = serial;
  std::vector<std::pair<uint32_t, uint32_t>> hits;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    hits.swap(watch_hits_);
  }
  for (auto& [key, texture] : guest_textures_) {
    if (!texture.watched) continue;
    for (const auto& [start, length] : hits) {
      if ((texture.watch_begin[0] < start + length && start < texture.watch_end[0]) ||
          (texture.watch_begin[1] < start + length && start < texture.watch_end[1])) {
        texture.watched = false;
        break;
      }
    }
  }
  for (auto& [key, kept] : kept_streams_) {
    if (!kept.valid) continue;
    for (const auto& [start, length] : hits) {
      if (kept.base < start + length && start < kept.base + kept.size) {
        // Earlier draws of this frame, and the frame before, may still read it.
        retired_buffers_[slot_].push_back(std::move(kept.buffer));
        kept_bytes_ -= kept.size;
        kept.valid = false;
        ++kept.drops;
        kept.dropped_frame = swaps_;
        ++stats_.kept_drops;
        break;
      }
    }
  }
}

bool PlumeCommandProcessor::GuestStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                        RenderBuffer*& buffer, uint64_t& offset) {
  // Streams are kept up to this much in all; past it they are copied as before.
  constexpr uint64_t kKeptBudget = 384ull << 20;
  buffer = upload_.get();
  if (!keep_streams_ || !address || !size || size > (32u << 20)) {
    return UploadGuest(address, size, endian, offset);
  }
  DrainWatchHits();
  // Kept by aligned window rather than by the exact stream: every draw of a
  // mesh names its own stretch of the same index buffer, and one entry per
  // stretch made tens of thousands of kept buffers, each looked up and each
  // checked against every write.
  constexpr uint32_t kWindow = 16u << 10;
  const uint32_t window_base = address & ~(kWindow - 1);
  const uint64_t window_end =
      std::min<uint64_t>((uint64_t(address) + size + kWindow - 1) & ~uint64_t(kWindow - 1), 0x20000000ull);
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
  kept.valid = true;
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
  auto idle_for = [&](bool short_of) { return short_of ? uint64_t(60) : uint64_t(900); };
  auto idle = [&](uint64_t frame, uint64_t limit) { return frame == ~0ull || swaps_ - frame >= limit; };
  uint32_t evicted = 0;

  const uint64_t limit_2d = idle_for(free_2d < 512);
  for (auto it = guest_textures_.begin(); it != guest_textures_.end();) {
    if (!idle(it->second.checked_frame, limit_2d)) { ++it; continue; }
    RetireTexture(it->second.texture, it->second.descriptor);
    it = guest_textures_.erase(it);
    ++evicted;
  }
  // Once, not per erased entry: the memo is 64 KB.
  texture_memo_.Clear();
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
    it = kept_streams_.erase(it);
    kept_erased = true;
  }
  if (kept_erased) kept_memo_.Clear();
  if (evicted) {
    REXLOG_INFO("Renut: Plume evicted {} idle textures (free descriptors 2D {} 3D {} cube {})", evicted,
                free_2d, free_3d, free_cube);
  }
}

void PlumeCommandProcessor::BeginFrame() {
  slot_ = (slot_ + 1) % kPlumeFramesInFlight;
  merge_frame_ = merge_bands_ && band_h_ && band_full_h_ > band_h_ && swaps_ - band_seen_frame_ <= 2;
  band2_ = false;
  // The slot's command list, ring slice and retired resources are all still in
  // use until the frame that last held it has finished.
  const auto wait_start = std::chrono::steady_clock::now();
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForSlot(slot_);
  stats_.slot_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - wait_start)
                                      .count());
  ReadTimestamps(slot_);
  ReadPairTimestamps(slot_);
  ApplyWritebacks(slot_);
  list_ = lists_[slot_].get();
  slice_base_ = slice_origin_ + uint64_t(slot_) * kUploadSize;
  for (uint32_t index : retired_descriptor_slots_[slot_]) {
    set_2d_->setTexture(index, white_2d_.get(), RenderTextureLayout::SHADER_READ);
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
  retired_framebuffer_slots_[slot_].clear();
  retired_slots_[slot_].clear();
  retired_views_[slot_].clear();
  retired_buffers_[slot_].clear();
  if (swaps_ - last_eviction_ >= 30) {
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

Resolved* PlumeCommandProcessor::FindResolvedContaining(uint32_t address, RenderFormat format,
                                                        uint32_t bytes_per_pixel, uint32_t pitch,
                                                        uint32_t height, uint32_t& column, uint32_t& row) {
  for (auto& [key, resolved] : resolved_) {
    const uint32_t base = uint32_t(key);
    if (!resolved.texture || resolved.format != format || address <= base) continue;
    // Only join bands of the same guest surface. Different guest formats can
    // share a host format (for example an 8-bit mask stored in RGBA8). Testing
    // the incoming bpp against an old image's host extent aliases unrelated
    // allocations, notably Banjo's mask and the adjacent golden aura.
    if (resolved.guest_bpp != bytes_per_pixel || resolved.guest_pitch != pitch ||
        resolved.guest_height != height) continue;
    const uint32_t offset = address - base;
    if (offset >= pitch * height * bytes_per_pixel) continue;
    if (resolved.written_frame != swaps_ && resolved.guest_bytes &&
        ContentHash(memory_->TranslatePhysical<const uint8_t*>(base), resolved.guest_bytes) !=
            resolved.guest_hash) continue;
    // Resolve addresses use Xenos tiled memory, not linear scanlines. The
    // persistent far shadow map is assembled from 512x512 rectangles over
    // multiple frames; horizontal tiles must land in columns of the same image.
    const uint32_t bpp_log2 = 31 - __builtin_clz(bytes_per_pixel);
    for (uint32_t y = 0; y < height; y += 32) {
      for (uint32_t x = 0; x < pitch; x += 32) {
        if (uint32_t(gfx::texture_util::GetTiledOffset2D(x, y, pitch, bpp_log2)) == offset) {
          column = x;
          row = y;
          return &resolved;
        }
      }
    }
  }
  return nullptr;
}

void PlumeCommandProcessor::MergeResolvedTails(uint32_t address, Resolved& dest) {
  // The static shadow atlas is built bottom-to-top. Each row's base points
  // inside the allocation and its height describes the remaining rows. When
  // an earlier base arrives, preserve the already-rendered tail in that image.
  const uint32_t bpp_log2 = 31 - __builtin_clz(dest.guest_bpp);
  for (auto it = resolved_.begin(); it != resolved_.end();) {
    auto& tail = it->second;
    const uint32_t tail_address = uint32_t(it->first);
    if (&tail == &dest || !tail.texture || tail_address <= address ||
        tail.format != dest.format || tail.guest_bpp != dest.guest_bpp ||
        tail.guest_pitch != dest.guest_pitch || !tail.guest_height ||
        tail.guest_height >= dest.guest_height) { ++it; continue; }
    const uint32_t y = dest.guest_height - tail.guest_height;
    if (y % 32 || tail_address - address != uint32_t(gfx::texture_util::GetTiledOffset2D(
        0, y, dest.guest_pitch, bpp_log2)) || y + tail.height > dest.height ||
        tail.width > dest.width) { ++it; continue; }
    if (tail.guest_bytes && ContentHash(memory_->TranslatePhysical<const uint8_t*>(tail_address),
                                      tail.guest_bytes) != tail.guest_hash) { ++it; continue; }
    Transition(tail.texture.get(), tail.layout, RenderTextureLayout::COPY_SOURCE);
    Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
    RenderBox box(0, 0, int32_t(tail.width), int32_t(tail.height), 0, 1);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
        RenderTextureCopyLocation::Subresource(tail.texture.get()), 0, y, 0, &box);
    dest.generation += tail.generation;
    RetireSwizzled(tail);
    RetireTexture(tail.texture, tail.descriptor);
    RetireTexture(tail.fitted, tail.fitted_descriptor);
    it = resolved_.erase(it);
    InvalidateResolvedLookups();
  }
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
    RenderTextureViewDesc desc = RenderTextureViewDesc::Texture2D(r.format);
    desc.componentMapping = RenderComponentMapping(lane(swizzle & 7), lane((swizzle >> 3) & 7),
                                                   lane((swizzle >> 6) & 7), lane((swizzle >> 9) & 7));
    s.view = texture->createTextureView(desc);
    s.descriptor = AllocateTextureDescriptor();
    s.texture = texture;
    if (!s.view || !s.descriptor) return 0;
    set_2d_->setTexture(s.descriptor, texture, RenderTextureLayout::SHADER_READ, s.view.get());
  }
  return s.descriptor;
}

uint32_t PlumeCommandProcessor::FittedResolved(Resolved& r, uint32_t width, uint32_t height) {
  if (!r.fitted || r.fitted_width != width || r.fitted_height != height) {
    RetireSwizzled(r);
    RetireTexture(r.fitted, r.fitted_descriptor);
    r.fitted = device_->createTexture(RenderTextureDesc::Texture2D(width, height, 1, r.format));
    r.fitted_width = width;
    r.fitted_height = height;
    r.fitted_layout = RenderTextureLayout::UNKNOWN;
    r.fitted_generation = UINT64_MAX;
    r.fitted_descriptor = AllocateTextureDescriptor();
    if (!r.fitted || !r.fitted_descriptor) return 0;
    set_2d_->setTexture(r.fitted_descriptor, r.fitted.get(), RenderTextureLayout::SHADER_READ);
  }
  if (r.fitted_generation != r.generation) {
    Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
    Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::COPY_DEST);
    RenderBox box(0, 0, int32_t(width), int32_t(height), 0, 1);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.fitted.get()),
                             RenderTextureCopyLocation::Subresource(r.texture.get()), 0, 0, 0, &box);
    r.fitted_generation = r.generation;
  }
  Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::SHADER_READ);
  return r.fitted_descriptor;
}

Target* PlumeCommandProcessor::FindTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch) {
  Target* best = nullptr;
  for (auto& [key, target] : targets_) {
    if ((key & 0xFFF) == base && ((key >> 12) & 0xF) == format && bool((key >> 16) & 1) == depth) {
      if (pitch && target.width == pitch) return &target;
      if (!best || target.width * target.height > best->width * best->height) best = &target;
    }
  }
  return best;
}

Target* PlumeCommandProcessor::GetTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch,
                                         uint32_t height) {
  uint64_t key = uint64_t(base & 0xFFF) | (uint64_t(format & 0xF) << 12) | (uint64_t(depth) << 16) |
                 (uint64_t(pitch) << 32);
  Target& target = targets_[key];
  target.last_used = ++target_serial_;
  if (target.texture && target.height >= height) return &target;
  // New, or taller than before: (re)create. Contents are lost on growth.
  uint32_t new_height = std::max({height, target.height, 64u});
  RenderFormat rf = depth ? RenderFormat::D32_FLOAT_S8_UINT
                          : ColorFormat(xenos::ColorRenderTargetFormat(format));
  RenderTextureDesc desc = depth ? RenderTextureDesc::DepthTarget(pitch, new_height, rf)
                                 : RenderTextureDesc::ColorTarget(pitch, new_height, rf);
  if (target.texture) retired_slots_[slot_].push_back(std::move(target.texture));
  target.texture = device_->createTexture(desc);
  if (!target.texture) {
    targets_.erase(key);
    return nullptr;
  }
  target.format = rf;
  target.width = pitch;
  target.height = new_height;
  target.depth = depth;
  target.layout = RenderTextureLayout::UNKNOWN;
  // Framebuffers referencing the old texture are stale.
  for (auto& [key, framebuffer] : framebuffers_)
    retired_framebuffer_slots_[slot_].push_back(std::move(framebuffer));
  framebuffers_.clear();
  bound_framebuffer_ = nullptr;
  return &target;
}

RenderFramebuffer* PlumeCommandProcessor::GetFramebuffer(const std::array<Target*, 4>& colors,
                                                         uint32_t color_count, Target* depth) {
  // What SetFramebuffer labels the next pass with.
  color_count_of_pass_ = color_count;
  depth_of_pass_ = depth != nullptr;
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
  return framebuffer.get();
}

void PlumeCommandProcessor::SetFramebuffer(RenderFramebuffer* framebuffer) {
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
    ++stats_.passes;
    PassMark mark;
    mark.width = framebuffer->getWidth();
    mark.height = framebuffer->getHeight();
    mark.colors = color_count_of_pass_;
    mark.depth = depth_of_pass_;
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
const PlumeCommandProcessor::FetchLayout* PlumeCommandProcessor::LayoutOf(
    const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint64_t fetch_key = 0;
  HashCombine(fetch_key, fetch.dword_0);
  HashCombine(fetch_key, fetch.dword_1);
  HashCombine(fetch_key, fetch.dword_2);
  HashCombine(fetch_key, fetch.dword_3);
  HashCombine(fetch_key, fetch.dword_4);
  HashCombine(fetch_key, fetch.dword_5);
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
  GuestTexture* texture_node = texture_memo_.Find(key);
  if (!texture_node) {
    texture_node = &guest_textures_[key];
    texture_memo_.Put(key, texture_node);
  }
  GuestTexture& texture = *texture_node;
  if (texture.failed) return 0;
  // Checked once per frame; the draws after the first reuse the result. A
  // texture whose memory is write-watched and was not written since is not
  // checked at all - hashing every texture in use every frame was a tenth of
  // this thread - except once every 256 frames, in case a write went around
  // the watch.
  DrainWatchHits();
  const bool unchanged =
      texture.checked_frame == swaps_ || (texture.watched && ((swaps_ + key) & 255) != 0);
  if (texture.texture && unchanged && texdump_directory_.empty()) {
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
    }
    texture.watched = true;
  }
  for (uint32_t i = 0; i < levels; ++i) HashCombine(content, ContentHash(data[i], layouts[i].extent));
  if (!texdump_directory_.empty() && dumped_.insert(key).second) texture.content = ~content;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> rgba;
  const uint32_t width = layouts[0].width, height = layouts[0].height;
  if (!DecodeTextureLevel(fetch, layouts[0], data[0], 0, rgba)) {
    texture.failed = true;
    ++stats_.texture_failures;
    if (unsupported_formats_.insert(uint32_t(fetch.format)).second) {
      REXLOG_WARN("Renut: Plume does not decode texture format {} yet", uint32_t(fetch.format));
    }
    return 0;
  }
  if (!texdump_directory_.empty()) DumpTexture(fetch, rgba, width, height, "2d");
  // New contents reuse the image when its shape is unchanged: earlier draws of
  // this frame may still reference it, and the upload is ordered after them.
  if (!texture.texture || texture.levels != levels || texture.width != width || texture.height != height) {
    RetireTexture(texture.texture, texture.descriptor);
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture2D(width, height, levels, RenderFormat::R8G8B8A8_UNORM));
    texture.levels = levels;
    texture.width = width;
    texture.height = height;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
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
  for (uint32_t level = 0; level < levels; ++level) {
    if (level && !DecodeTextureLevel(fetch, layouts[level], data[level], 0, rgba)) return 0;
    w = layouts[level].width;
    h = layouts[level].height;
    const uint64_t staging = Allocate(uint64_t(w) * h * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), rgba.data(), size_t(w) * h * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), level),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, w, h, 1, w, staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_2d_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_.textures_decoded;
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
    register_file_->values[index] = value;
    MarkConstantsWritten(index, 1);
    return;
  }
  CommandProcessor::WriteRegister(index, value);
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
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

gfx::Shader* PlumeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t,
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
  mkdir((dir + "/ir").c_str(), 0755);
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

bool PlumeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info, bool) {
  const gfx::RegisterFile& regs = *register_file_;
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) return IssueCopy();
  ++stats_.draws;
  const uint32_t draw_index = frame_draw_++;
  if (draw_index >= drawskip_first_ && draw_index < drawskip_end_) return true;

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
  // Predicated tiling (see merge_bands_). A negative window offset is the
  // replay of a lower band: learn its layout, and while merging drop it.
  const int32_t band_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset;
  const uint32_t band_surface_pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  const uint32_t band_color = regs.Get<reg::RB_COLOR_INFO>().color_base;
  if (band_offset < 0 && edram_mode == xenos::EdramMode::kColorDepth && band_surface_pitch) {
    // The band's own height is the scissor's, not the window scissor's (that
    // one stays at the upper band's 384 rows).
    draw_util::Scissor band_scissor;
    draw_util::GetScissor(regs, band_scissor);
    const uint32_t below = uint32_t(-band_offset) + band_scissor.offset[1] + band_scissor.extent[1];
    if (band_color != band_color_base_ || band_surface_pitch != band_pitch_ ||
        uint32_t(-band_offset) != band_h_) {
      band_color_base_ = band_color;
      band_depth_base_ = regs.Get<reg::RB_DEPTH_INFO>().depth_base;
      band_pitch_ = band_surface_pitch;
      band_h_ = uint32_t(-band_offset);
      band_full_h_ = below;
      REXLOG_INFO("Renut: Plume predicated tiling: bands of {} rows over {} (colour {:X}, depth {:X}, pitch {})",
                  band_h_, band_full_h_, band_color_base_, band_depth_base_, band_pitch_);
    }
    band_full_h_ = std::max(band_full_h_, below);
    band_seen_frame_ = swaps_;
  }
  const bool band_surface =
      merge_frame_ && band_surface_pitch == band_pitch_ && band_color == band_color_base_;
  if (band_surface && band_offset < 0 && uint32_t(-band_offset) == band_h_) {
    band2_ = true;
    ++stats_.band_skipped;
    return true;
  }
  // Upper band: full-screen draws widen to the whole target. Lower band (after
  // its replay began): the game's own draws move down by the band's height.
  const bool band_merge = band_surface && band_offset == 0;
  // Direct3D's Clear draws a rectangle with its own shader, in an EDRAM view
  // of the targets (half the pitch at 64 bpp, all four colour targets, a
  // 4096-row surface) that only aliases the real ones on the hardware. Drawn
  // literally it would clear render targets of its own; clear the real ones.
  if (vs->entry->hash == kD3DClearVertexShader && prim_type == xenos::PrimitiveType::kRectangleList &&
      !(skip_mask_ & 2048)) {
    // A clear that starts the lower band's replay would wipe what the upper
    // band now draws for the whole screen.
    if (merge_frame_ && band_offset < 0 && uint32_t(-band_offset) == band_h_) return true;
    IssueD3DClear(edram_mode);
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

  // Render targets.
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t pitch = surface.surface_pitch;
  if (!pitch) return Drop("no pitch", vs, ps);
  const auto window_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  const uint32_t height =
      std::clamp<uint32_t>(std::max<uint32_t>(window_br.br_y, band_merge ? band_full_h_ : 0u), 16, 4096);
  max_draw_height_ = std::max(max_draw_height_, height);
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
      colors[i] = GetTarget(info.color_base, uint32_t(info.color_format), false, pitch, height);
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
    depth = GetTarget(depth_info.depth_base, uint32_t(depth_info.depth_format), true, pitch, height);
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
  // GetTarget may have recreated a texture that an earlier slot also uses.
  for (uint32_t i = 0; i < color_count; ++i) {
    Transition(colors[i]->texture.get(), colors[i]->layout, RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);

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
      const uint32_t fw = fetch.size_2d.width + 1, fh = fetch.size_2d.height + 1;
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
      texture_descriptor[slot] = GetGuestTexture(fetch);
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
    Trace("  depthcontrol={:08X} stencilrefmask={:08X} bf={:08X}", regs[XE_GPU_REG_RB_DEPTHCONTROL],
          regs[XE_GPU_REG_RB_STENCILREFMASK], regs[XE_GPU_REG_RB_STENCILREFMASK_BF]);
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
    if (BindStream(fetch.address << 2, fetch.size << 2, fetch.endian, buffer, offset)) {
      streams[stream] = buffer == upload_.get() ? Address(offset) : buffer->getDeviceAddress() + offset;
      slot_offsets[stream] = offset;
      slot_buffers[stream] = buffer;
      slot_sizes[stream] = fetch.size << 2;
    }
  }
  std::memcpy(sh + RENUT_SHARED_STREAMS_OFFSET, streams, sizeof(streams));

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
  // Window offset (predicated tiling moves each band to the target's origin)
  // and the D3D9 pixel centre at integer coordinates.
  float window_x = 0.f, window_y = 0.f;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    window_x = float(offset.window_x_offset);
    window_y = float(offset.window_y_offset);
  }
  if (band_merge && band2_) window_y += float(band_h_);
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
    viewport = RenderViewport(window_x, window_y, float(extent->width), float(extent->height));
    scale_offset[0] = 2.f / float(extent->width);
    scale_offset[1] = -2.f / float(extent->height);
    scale_offset[2] = -1.f;
    scale_offset[3] = 1.f;
  }
  if (viewport.width <= 0.f) {
    viewport = RenderViewport(0.f, 0.f, float(extent->width), float(extent->height));
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
  // Merged predicated tiling: pixels below the upper band read their position
  // relative to the lower band, as they did when it was drawn on its own
  // (renutFragPos). The upper band's full-screen draws and the game's own
  // lower-band draws need it; every other draw gets 0.
  float band_split =
      band_merge && (band2_ || std::fabs(viewport.height) >= float(band_full_h_) - 1.f) ? float(band_h_) : 0.f;
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

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  RenderRect scissor_rect(int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
                          int32_t(scissor.offset[0] + scissor.extent[0]),
                          int32_t(scissor.offset[1] + scissor.extent[1]));
  if (band_merge && band2_) {
    scissor_rect.top += int32_t(band_h_);
    scissor_rect.bottom = std::min(scissor_rect.bottom + int32_t(band_h_), int32_t(band_full_h_));
    ++stats_.band_moved;
  } else if (band_merge && scissor_rect.bottom == int32_t(band_h_) &&
             std::fabs(viewport.height) >= float(band_full_h_) - 1.f) {
    scissor_rect.bottom = int32_t(band_full_h_);
    ++stats_.band_widened;
  }
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
  const uint32_t* vs_bools = bools_of(vs);
  const uint32_t* ps_bools = bools_of(ps);
  for (uint32_t word = 0; word < 8; ++word) {
    const uint32_t tested = vs_bools[word] | (ps_bools ? ps_bools[word] : 0);
    const uint32_t value = regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + word] & tested;
    if (value) spec_constants.emplace_back(RENUT_SPEC_BOOLS_BASE + word, value);
  }
  // The live vertex fetch table, if the shader's module has a form that reads
  // it from specialization constants (spirv_fetch_spec.h): what the table holds
  // comes from the shader's own vfetch instructions, so it is the same on every
  // draw that uses this shader and adds no pipelines of its own.
  CompiledShader* vertex_compiled = patch_list ? vs->domain_compiled : vs->compiled;
  const bool fetch_spec = fetch_spec_mode_ && vertex_compiled && vertex_compiled->fetch_spec_module;
  if (fetch_spec) {
    for (const auto& [slot, entry] : patch_list ? vs->domain_live_fetches : vs->live_fetches) {
      for (uint32_t word = 0; word < 4; ++word) {
        if (entry[word]) spec_constants.emplace_back(kFetchSpecBase + slot * 4 + word, entry[word]);
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
  for (const RenderSpecConstant& c : spec_constants) {
    key_bytes.Put(c.index);
    key_bytes.Put(c.value);
  }
  // Mode 8 swaps a point list's pixel shader for the sprite-coordinate view.
  const bool debug_point = debug_draw && (debug_mode_ & 8) && point_list && color_count;
  // Geometry-shader and patch draws keep their own: their stages expect the
  // guest's outputs.
  const bool plain_draw = !rect_list && !point_list && !patch_list;
  // A draw with no fragment shader reads none of the vertex shader's outputs
  // but its position, so it can have the form that does not write them
  // (spirv_depth_only.h).
  RenderShader* depth_module = nullptr;
  if (depth_strip_mode_ && !ps && plain_draw && vertex_compiled) {
    depth_module = fetch_spec ? vertex_compiled->depth_fetch_spec_module.get() : vertex_compiled->depth_module.get();
  }
  const bool depth_strip = depth_module != nullptr;
  key_bytes.Put(uint8_t(fetch_spec | (depth_strip << 1)));
  // Debug (plume-trivial bit 4): a trivial vertex shader for depth-only draws
  // alone, to price what those draws cost without their vertex work.
  const bool trivial_vs = ((trivial_mode_ & 1) || ((trivial_mode_ & 4) && !ps)) && plain_draw;
  const bool trivial_fs = ps && (trivial_vs || ((trivial_mode_ & 2) && plain_draw));
  key_bytes.Put(uint8_t(debug_point | (trivial_vs << 1) | (trivial_fs << 2)));
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
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = trivial_vs   ? debug_trivial_vs_.get()
                        : depth_strip ? depth_module
                        : fetch_spec  ? vertex_compiled->fetch_spec_module.get()
                                      : vertex_compiled->module.get();
    desc.geometryShader = rect_list ? rect_list_gs_.get() : point_list ? point_list_gs_.get() : nullptr;
    desc.pixelShader = debug_point  ? debug_point_fs_.get()
                       : trivial_fs ? debug_trivial_fs_.get()
                       : ps         ? ps->compiled->module.get()
                                    : nullptr;
    desc.primitiveTopology = topology;
    desc.cullMode = cull;
    desc.frontFace = front_face;
    desc.depthClipEnabled = true;
    desc.depthEnabled = depth_enabled;
    desc.depthWriteEnabled = depth_write;
    desc.depthFunction = depth_enabled ? Compare(depth_control.zfunc) : RenderComparisonFunction::ALWAYS;
    desc.depthTargetFormat = depth ? depth->format : RenderFormat::UNKNOWN;
    desc.dynamicBlendFactorEnabled = blend_constants;
    desc.stencilEnabled = stencil_enabled;
    if (stencil_enabled) {
      desc.dynamicStencilEnabled = true;
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
    // Exporting shaders feed later draws and sometimes the guest CPU. Never
    // skip those draws merely because their first pipeline is still compiling.
    if (REXCVAR_GET(renut_plume_async_pipelines) &&
        !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) {
      auto pending = pending_pipelines_.find(key);
      if (pending != pending_pipelines_.end()) {
        if (!pending->second->done.load(std::memory_order_acquire))
          return Drop("pipeline compiling", vs, ps);
        pipeline = std::move(pending->second->result);
        stats_.pipeline_us += pending->second->duration_us;
        ++stats_.pipelines;
        pending_pipelines_.erase(pending);
        if (!pipeline) failed_pipelines_.insert(key);
      } else if (!failed_pipelines_.count(key) && pending_pipelines_.size() < 128) {
        auto job = std::make_shared<PipelineJob>();
        job->desc = desc;
        job->elements = input_elements;
        job->slots = input_slots;
        job->constants = spec_constants;
        StartPipelineWorker();
        pending_pipelines_.emplace(key, job);
        {
          std::lock_guard lock(pipeline_mutex_);
          pipeline_queue_.push_back(std::move(job));
        }
        pipeline_ready_.notify_one();
        return Drop("pipeline compiling", vs, ps);
      }
    }
    if (!pipeline && !failed_pipelines_.count(key) && !pending_pipelines_.count(key)) {
      const auto pipeline_start = std::chrono::steady_clock::now();
      pipeline = device_->createGraphicsPipeline(desc);
      stats_.pipeline_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - pipeline_start)
                                         .count());
      ++stats_.pipelines;
    }
    if (pipeline && getenv("RENUT_PIPELINE_STATS")) LogPipelineStatistics(*pipeline, vs, ps);
    if (pipeline && getenv("RENUT_PIPELINE_IR")) {
      DumpPipelineInternals(*pipeline, vs, ps, miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')));
    }
    if (!pipeline) {
      REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})",
                  vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0);
    }
  }
  if (!pipeline) {
    ++stats_.bad_shader;
    return true;
  }

  // Indices.
  uint32_t vertex_count = index_count;
  bool indexed = index_buffer_info && index_buffer_info->guest_base;
  RenderFormat index_format = RenderFormat::R16_UINT;
  uint64_t index_offset = 0;
  RenderBuffer* index_buffer = upload_.get();
  uint32_t index_bytes = 0;
  const PatchGrid* patch_grid = nullptr;
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
    patch_grid = GetPatchGrid(quad_patch, level, patches);
    if (!patch_grid) return Drop("patch grid", vs, ps);
    vertex_count = patches * patch_grid->indices_per_patch;
  }
  if (indexed) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    index_format = is32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT;
    const uint32_t size = index_count * (is32 ? 4 : 2);
    // A quad list's indices are read back here to be split into triangles,
    // so they stay in the ring.
    const bool uploaded =
        quad_list ? UploadGuest(index_buffer_info->guest_base, size, index_buffer_info->endianness, index_offset)
                  : GuestStream(index_buffer_info->guest_base, size, index_buffer_info->endianness,
                                index_buffer, index_offset);
    if (!uploaded) return Drop("index upload", vs, ps);
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
  pk.pipeline = pipeline.get();
  pk.interface_key = interface_key;
  pk.vs_hash = vs->entry->hash ^ (uint64_t(rel_constants_mode_ >= 10 ? rel_variant : 0) << 60);
  pk.ps_hash = ps ? ps->entry->hash : 0;
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
  pk.scissor = scissor_rect;
  draw_slots_.clear();
  for (const auto& slot : input_slots) {
    DeferredSlot entry;
    entry.slot = slot;
    if (patch_list) {
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
  if (defer_mode_ && !trace_ && !skip_mask_ && !bind_skip_ && !trivial_mode_ && !debug_draw && !patch_list &&
      !merge_frame_ && !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT) && depth_write) {
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
    if (defer) {
      HashCombine(window_signature, framebuffer);
      HashCombine(window_signature, color_mask);
      HashCombine(window_signature, uint32_t(depth_control.value));
      HashCombine(window_signature, stencil_enabled ? uint32_t(stencil_ref_mask.stencilwritemask) : 0u);
      HashCombine(window_signature, uint32_t(edram_mode));
    }
  }
  if (defer) {
    if (!deferred_.empty() && deferred_signature_ != window_signature) FlushDeferred();
    deferred_signature_ = window_signature;
    pk.first_slot = uint32_t(deferred_slots_.size());
    deferred_slots_.insert(deferred_slots_.end(), draw_slots_.begin(), draw_slots_.end());
    deferred_.push_back(pk);
    ++stats_.deferred_draws;
    if (deferred_.size() >= kMaxDeferred) FlushDeferred();
    return true;
  }
  FlushDeferred();
  EmitDraw(pk, draw_slots_.data(), uint32_t(draw_slots_.size()));
  return true;
}

// Records one packet: the pass, the pipeline and every piece of per-draw state
// that differs from what the list already has bound, then the draw itself.
void PlumeCommandProcessor::EmitDraw(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count) {
  const bool pair_stamped = pair_capture_ && PairBegin(pk);
  color_count_of_pass_ = pk.pass_colors;
  depth_of_pass_ = pk.pass_depth;
  SetFramebuffer(pk.framebuffer);
  // plume-skip bit 262144 keeps the bound pipeline whenever it can stand in
  // for the draw's own (same attachments and vertex input): the picture is
  // wrong, and what the GPU saves is the price of switching programs.
  const bool keep_pipeline = (skip_mask_ & 262144) && bound_pipeline_ && pk.interface_key == bound_interface_key_;
  if (bound_pipeline_ != pk.pipeline && !keep_pipeline) {
    L()->setPipeline(pk.pipeline);
    bound_pipeline_ = pk.pipeline;
    bound_interface_key_ = pk.interface_key;
    ++stats_.pipeline_binds;
  }
  if (pk.blend_constants) L()->setBlendFactor(pk.blend_factor);
  if (pk.stencil) L()->setStencilState(pk.stencil_ref, pk.stencil_mask, pk.stencil_write_mask);
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
  switch (pk.kind) {
    case DrawPacket::Kind::kNone: break;
    case DrawPacket::Kind::kIndexed:
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      L()->drawIndexedInstanced(pk.count, 1, pk.first, pk.base_vertex, 0);
      break;
    case DrawPacket::Kind::kPatch:
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      L()->drawIndexedInstanced(pk.count, 1, 0, 0, 0);
      break;
    case DrawPacket::Kind::kPlain: L()->drawInstanced(pk.count, 1, pk.first, 0); break;
  }
  if (pair_stamped) PairStamp();
  ++stats_.drawn;
  if (!pass_marks_[slot_].empty()) ++pass_marks_[slot_].back().draws;
}

// Records the draws that are waiting, grouped by pipeline. The groups keep the
// order in which each pipeline first appeared and the draws keep their order
// inside a group, so a window whose draws share one pipeline is untouched.
void PlumeCommandProcessor::FlushDeferred() {
  if (deferred_.empty()) return;
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
  for (size_t i = 0; i < n; ++i) {
    const DrawPacket& pk = flushing_[defer_order_[i]];
    EmitDraw(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
  }
  flushing_.clear();
  flushing_slots_.clear();
}

bool PlumeCommandProcessor::IssueCopy() {
  FlushDeferred();
  const gfx::RegisterFile& regs = *register_file_;
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) return true;
  ++stats_.resolves;
  // plume-skip bit 33554432 drops the resolve entirely. Attributing intervals
  // between timestamps proved unreliable on this tiler - the same frame read as
  // 83% resolve or 89% between-passes depending only on which side of the
  // pass-closing barrier the mark sat. Removing a category of work and
  // measuring the difference does not depend on that.
  if (skip_mask_ & 33554432) return true;
  const uint32_t width = info.coordinate_info.width_div_8 * 8;
  const uint32_t height = info.height_div_8 * 8;
  if (!width || !height) return true;

  const bool copying_depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram = copying_depth ? info.depth_edram_info : info.color_edram_info;
  const uint32_t original = copying_depth ? info.depth_original_base : info.color_original_base;
  // Source rectangle in pixels: whole tiles from the render target's base,
  // plus the offset within the first tile.
  // A tile is 80x16 samples. Targets here hold one texel per pixel whatever
  // the guest's sample count, so a 4x tile is 40 pixels wide and a 2x or 4x
  // one 8 tall. Counting samples as pixels found the dialogue portraits (96x96
  // at 4x, pitch 120) and the blueprint (256x256 at 4x, pitch 280) in the
  // twice-as-wide 1x target sharing their EDRAM base: no portrait, and a
  // quarter of the vehicle.
  auto tile_size = [](const draw_util::ResolveEdramInfo& e, uint32_t& tw, uint32_t& th) {
    tw = (e.format_is_64bpp ? 40 : 80) >> (e.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0);
    th = 16 >> (e.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0);
  };
  uint32_t tile_width, tile_height;
  tile_size(edram, tile_width, tile_height);
  // EDRAM bases are shared between targets of different pitches (both shadow
  // maps use depth base 0), so prefer the one the resolve's pitch names.
  Target* source = FindTarget(original, edram.format, copying_depth,
                              std::max(1u, uint32_t(edram.pitch_tiles)) * tile_width);
  const uint32_t tiles = edram.base_tiles - original;
  const uint32_t pitch_tiles = std::max(1u, uint32_t(edram.pitch_tiles));
  const uint32_t x = (tiles % pitch_tiles) * tile_width + info.coordinate_info.edram_offset_x_div_8 * 8;
  uint32_t y = (tiles / pitch_tiles) * tile_height + info.coordinate_info.edram_offset_y_div_8 * 8;
  // plume-skip bit 134217728 drops every clear, these and Direct3D's. The
  // picture is wrong; it measures what the clear-only passes cost.
  const bool skip_clears = skip_mask_ & 134217728;
  // Predicated tiling done once (merge_bands_): the upper band's resolves copy
  // the whole screen, which the upper band now draws, but clear only the band;
  // the lower band's read it from below the upper one.
  uint32_t copy_height = height;
  if (merge_frame_ && source && pitch_tiles * tile_width == band_pitch_ &&
      original == (copying_depth ? band_depth_base_ : band_color_base_)) {
    if (band2_) {
      y += band_h_;
    } else if (y == 0 && height == band_h_) {
      copy_height = band_full_h_;
    }
  }

  Trace("resolve {} base={:X} fmt={} src=({},{}) {}x{} found={} -> {:08X} dfmt={} ({},{}) pitch={} h={} clear={}{}",
        copying_depth ? "depth" : "color", original, uint32_t(edram.format), x, y, width, height,
        source != nullptr, uint32_t(info.copy_dest_base), uint32_t(info.copy_dest_info.copy_dest_format),
        info.copy_dest_coordinate_info.offset_x_div_8 * 8, info.copy_dest_coordinate_info.offset_y_div_8 * 8,
        info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
        info.copy_dest_coordinate_info.height_aligned_div_32 * 32, info.IsClearingColor() ? "c" : "",
        info.IsClearingDepth() ? "d" : "");
  if (source && x < source->width && y < source->height &&
      info.rb_copy_control.copy_command != xenos::CopyCommand::kNull) {
    const uint32_t w = std::min(width, source->width - x), h = std::min(copy_height, source->height - y);
    const uint32_t dest_w = std::max(uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32u, w);
    const uint32_t dest_h = std::max(uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32u, h);
    const uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
    const uint32_t dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
    // Screen bands and shadow tiles resolve to addresses inside one tiled
    // guest texture. Recover both coordinates, including across frames.
    uint32_t column = 0, row = 0;
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
    Resolved* inside = FindResolvedContaining(info.copy_dest_base, dest_format, dest_bpp,
                                              dest_w, dest_h, column, row);
    Resolved& dest = inside && inside->width >= column + dx + w && inside->height >= row + dy + h
                         ? *inside
                         : GetResolved(info.copy_dest_base, dest_format, dest_w + dx, dest_h + dy);
    const uint32_t dest_y = &dest == inside ? dy + row : dy;
    const uint32_t dest_x = &dest == inside ? dx + column : dx;
    ++dest.generation;
    dest.depth = copying_depth;
    dest.written = ++resolve_serial_;
    InvalidateResolvedLookups();
    dest.written_frame = swaps_;
    dest.guest_pitch = dest_w;
    dest.guest_height = dest_h;
    dest.guest_bpp = dest_bpp;
    if (&dest != inside) MergeResolvedTails(info.copy_dest_base, dest);
    // Snapshot of the guest memory under the whole resolved texture.
    {
      uint32_t base_address = info.copy_dest_base;
      if (&dest == inside) base_address -= uint32_t(gfx::texture_util::GetTiledOffset2D(
          column, row, dest.guest_pitch, 31 - __builtin_clz(dest_bpp)));
      dest.guest_bytes = dest.guest_pitch * dest.guest_height * dest_bpp;
      dest.guest_hash = ContentHash(memory_->TranslatePhysical<const uint8_t*>(base_address), dest.guest_bytes);
      dest.checked_frame = swaps_;
    }
    if (dest.texture && copying_depth) {
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      CopyDepth(*source, x, y, w, h, dest.texture.get(), dest_x, dest_y, with_stencil && dest.format == dest_format);
    } else if (dest.texture) {
      Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      // plume-skip bit 268435456 keeps the resolve's barriers and pass breaks
      // but drops the copies themselves, colour and depth (see CopyDepth).
      if (!(skip_mask_ & 268435456)) {
        RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h), 0, 1);
        L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
                                 RenderTextureCopyLocation::Subresource(source->texture.get()), dest_x,
                                 dest_y, 0, &box);
      }
    }
    if (dest.texture) {
      if (snap_address_ && info.copy_dest_base == snap_address_ && ++snap_seen_ == snap_ordinal_) {
        SnapshotResolved(dest);
      }
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
      if (dest.descriptor) {
        set_2d_->setTexture(dest.descriptor, dest.texture.get(), RenderTextureLayout::SHADER_READ);
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
    uint32_t depth_tile_width, depth_tile_height;
    tile_size(info.depth_edram_info, depth_tile_width, depth_tile_height);
    Target* depth = FindTarget(info.depth_original_base, info.depth_edram_info.format, true,
                               std::max(1u, uint32_t(info.depth_edram_info.pitch_tiles)) * depth_tile_width);
    if (depth) {
      Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      SetFramebuffer(GetFramebuffer(none, 0, depth));
      const uint32_t d = info.rb_depth_clear;
      L()->clearDepthStencil(true, true, float(d >> 8) / float(0xFFFFFF), d & 0xFF, &rect, 1);
      ++stats_.clears;
    }
  }
  return true;
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

void PlumeCommandProcessor::IssueD3DClear(xenos::EdramMode edram_mode) {
  FlushDeferred();
  const gfx::RegisterFile& regs = *register_file_;
  // The rectangle's first vertex: float3 position (z is the clear depth) and
  // float4 colour, 28 bytes, through vertex fetch constant 0.
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.address || (fetch.size << 2) < 28) return;
  uint8_t vertex[28];
  CopySwapped(vertex, memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2), sizeof(vertex),
              fetch.endian);
  float v[7];
  std::memcpy(v, vertex, sizeof(v));
  if (skip_mask_ & 134217728) return;  // clears dropped, see IssueCopy
  ++stats_.clears;
  Trace("clear mode={} pitch={} depth={} color=({}, {}, {}, {}) colormask={:X}", uint32_t(edram_mode),
        uint32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch), v[2], v[3], v[4], v[5], v[6],
        regs[XE_GPU_REG_RB_COLOR_MASK]);

  // The whole target: Direct3D clears the current viewport, which for
  // N&B's predicated-tiling bands is the band - the target itself.
  static constexpr gfx::Register kColorInfo[4] = {XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                                                  XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO};
  // The clear's pitch is the target's, or half of it when Direct3D clears a
  // 32 bpp target as a 64 bpp one; EDRAM bases are shared between targets of
  // other pitches (the shadow maps), so match the pitch, not just the base.
  const uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  // Direct3D's Clear scissors the EDRAM view, which is 4096 rows tall, while
  // the game renders into a few hundred. GetTarget never shrinks a target, so
  // letting that height through grew every real target to 4096 rows and made a
  // tiler reload and resolve 21 MB of attachment at each of the hundred render
  // passes in a frame. Cap it by the tallest height a draw has actually asked
  // for; a draw that needs more still grows the target itself.
  uint32_t clear_height = std::clamp<uint32_t>(
      regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y, 16, 4096);
  if (!(skip_mask_ & 2097152)) {
    // The floor covers the clears that land before the first draw of a target,
    // among them the static shadow map's 512-row tiles.
    clear_height = std::min(clear_height, std::max(max_draw_height_, 1024u));
  }
  // Several targets can match (a pitch-640 target from another phase next to
  // the 1280-wide scene target that a 64 bpp clear also names): the one last
  // rendered to is the one the game is about to use.
  auto find = [&](uint32_t base, uint32_t format, bool depth) -> Target* {
    Target* best = nullptr;
    for (auto& [key, target] : targets_) {
      if ((key & 0xFFF) != base || ((key >> 12) & 0xF) != format || bool((key >> 16) & 1) != depth) continue;
      if (target.width != pitch && target.width != 2 * pitch) continue;
      if (!best || target.last_used > best->last_used) best = &target;
    }
    return best;
  };
  const uint32_t color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    std::array<Target*, 4> cleared{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((color_mask >> (4 * i)) & 0xF)) continue;
      reg::RB_COLOR_INFO info;
      info.value = regs[kColorInfo[i]];
      Target* target = find(info.color_base, uint32_t(info.color_format), false);
      // A target nothing has drawn to yet (N&B fills a 320x184 buffer with
      // point lists only, which are not drawn yet): create it, so the clear
      // lands and its resolve copies the clear colour instead of leaving the
      // texture to be decoded from memory the resolve never wrote.
      if ((!target && i == 0) || (target && target->height < clear_height)) {
        target = GetTarget(info.color_base, uint32_t(info.color_format), false,
                           target ? target->width : pitch, clear_height);
      }
      if (!target || std::find(cleared.begin(), cleared.end(), target) != cleared.end()) continue;
      cleared[i] = target;
      Trace("  clear colour rt{} base={:X} -> target {}x{}", i, uint32_t(info.color_base), target->width,
            target->height);
      Transition(target->texture.get(), target->layout, RenderTextureLayout::COLOR_WRITE);
      std::array<Target*, 4> colors{target};
      SetFramebuffer(GetFramebuffer(colors, 1, nullptr));
      const RenderRect rect(0, 0, int32_t(target->width), int32_t(target->height));
      L()->clearColor(0, RenderColor(v[3], v[4], v[5], v[6]), &rect, 1);
    }
  }
  const auto depth_control = draw_util::GetNormalizedDepthControl(regs);
  const bool clear_depth = depth_control.z_enable && depth_control.z_write_enable;
  const bool clear_stencil = depth_control.stencil_enable;
  if (clear_depth || clear_stencil) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    Target* depth = find(depth_info.depth_base, uint32_t(depth_info.depth_format), true);
    // Allocate the full scissor height before clearing. Growing a 256-row
    // placeholder on the first draw discarded the clear and blanked the first
    // 512x512 tile of the static shadow map.
    if (!depth || depth->height < clear_height)
      depth = GetTarget(depth_info.depth_base, uint32_t(depth_info.depth_format), true,
                        depth ? depth->width : pitch, clear_height);
    Trace("  clear depth={} stencil={} ref={:X} depthcontrol={:08X}", clear_depth, clear_stencil,
          regs.Get<reg::RB_STENCILREFMASK>().stencilref, regs[XE_GPU_REG_RB_DEPTHCONTROL]);
    Trace("  clear depth base={:X} -> {}", uint32_t(depth_info.depth_base),
          depth ? fmt::format("target {}x{}", depth->width, depth->height) : std::string("none"));
    if (depth) {
      Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      SetFramebuffer(GetFramebuffer(none, 0, depth));
      const RenderRect rect(0, 0, int32_t(depth->width), int32_t(depth->height));
      L()->clearDepthStencil(clear_depth, clear_stencil, std::clamp(v[2], 0.f, 1.f),
                               regs.Get<reg::RB_STENCILREFMASK>().stencilref, &rect, 1);
    }
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

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  auto* renderer = PlumeRenderer::Get();
  FlushDeferred();
  // Before anything is picked for presenting: the one that is moves on to
  // COPY_SOURCE below, which is a transition of its own.
  FlushImagesForNextFrame();
  RenderTexture* source = nullptr;
  Resolved* found = FindResolved(frontbuffer_ptr);
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
        L()->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(snap_dump_.get(), snap_format_, snap_width_,
                                                                            snap_height_, 1, snap_dump_row_),
                                 RenderTextureCopyLocation::Subresource(snap_.get()));
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
      mkdir(directory.c_str(), 0755);
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
      mkdir(texdump_directory_.c_str(), 0755);
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
      unsigned int minimum = 0;
      if (FILE* file = std::fopen((root + "/plume-shadowmin").c_str(), "r")) {
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
    bool merge_bands = false;
    if (FILE* file = std::fopen((root + "/plume-merge-bands").c_str(), "r")) {
      int value = 0;
      if (std::fscanf(file, "%d", &value) == 1) merge_bands = value != 0;
      std::fclose(file);
    }
    if (merge_bands != merge_bands_) REXLOG_INFO("Renut: Plume band merging {}", merge_bands ? "on" : "off");
    merge_bands_ = merge_bands;
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
    }
    skip_mask_ = mask;
    const bool keep_streams = access((root + "/plume-no-persist").c_str(), F_OK) != 0;
    if (keep_streams != keep_streams_) {
      REXLOG_INFO("Renut: Plume kept streams {}", keep_streams ? "on" : "off");
      if (!keep_streams) {
        for (auto& [key, kept] : kept_streams_) {
          if (kept.buffer) retired_buffers_[slot_].push_back(std::move(kept.buffer));
        }
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
    unsigned int trivial = 0;
    if (FILE* file = std::fopen((root + "/plume-trivial").c_str(), "r")) {
      if (std::fscanf(file, "%u", &trivial) != 1) trivial = 0;
      std::fclose(file);
    }
    if (trivial != trivial_mode_) REXLOG_INFO("Renut: Plume trivial shader mode {}", trivial);
    trivial_mode_ = trivial;
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
      it = exports_.erase(it);
    } else {
      ++it;
    }
  }
  if (swaps_ % 300 == 1) Report();
  BeginFrame();
}

void PlumeCommandProcessor::Report() {
  renutReportShadowEngineProfile();
  REXLOG_INFO(
      "Renut: Plume frame {}: draws={} drawn={} no_vs={} no_ps={} bad_shader={} skipped_prim={} "
      "no_target={} resolves={} clears={} passes={} barriers={} pipeline_binds={} vertices={} gs_draws={} patch_draws={} patch_vertices={} split(color={} depth={} read={} src={} dst={}) pipelines={} targets={} resolved={} shaders={} "
      "missing_ucode={} upload_overflow={} upload_used={}KB textures={} decoded={} "
      "texture_failures={} frame_ms={} pipeline_ms={} submit_ms={} guest_swaps={} "
      "const_reused={} export(draws={} ranges={}/{} reads={} merges={} refreshes={} barriers={}) "
      "cp_cpu_ms={} slot_wait_ms={} stream_mb={} constant_mb={} kept(hits={} new_mb={} drops={} unwatchable={} total_mb={} streams={}) "
      "bands(skipped={} moved={} widened={})",
      swaps_, stats_.draws, stats_.drawn, stats_.no_vs, stats_.no_ps, stats_.bad_shader,
      stats_.skipped_prim, stats_.no_target, stats_.resolves, stats_.clears, stats_.passes, stats_.barriers,
      stats_.pipeline_binds, stats_.vertices, stats_.gs_draws, stats_.patch_draws, stats_.patch_vertices,
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
  if (stats_.defer_windows) {
    REXLOG_INFO("Renut: Plume deferred draws (mode {}): {} draws in {} windows, pipeline changes {} -> {}",
                defer_mode_, stats_.deferred_draws, stats_.defer_windows, stats_.defer_binds_before,
                stats_.defer_binds_after);
  }
  if (shadow_min_indices_) {
    REXLOG_INFO("Renut: Plume shadow casters under {} indices dropped: {}", shadow_min_indices_,
                stats_.shadow_culled);
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
  if (stats_.pipelines) SavePlumePipelineCache();
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
  reported_guest_swaps_ = renut_guest_swap_count();
  stats_ = {};
}

// ---------------------------------------------------------------------------
// Graphics system
// ---------------------------------------------------------------------------

class PlumeGraphicsSystem final : public gfx::GraphicsSystem {
 public:
  std::string name() const override { return "Plume"; }

 protected:
  // Presentation belongs to PlumeRenderer; there is no RexGlue provider.
  void CreateProvider(bool) override {}
  std::unique_ptr<gfx::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<PlumeCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace

rex::system::IGraphicsSystem* CreatePlumeGraphicsSystem() { return new PlumeGraphicsSystem(); }

void SetPlumeMissDumpDirectory(const std::string& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  g_miss_dump_directory = directory;
}

}  // namespace renut
