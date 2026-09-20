#include "plume_gpu.h"

#include "plume_renderer.h"
#include "plume_textures.h"
#include "plume_vulkan.h"
#include "shader_cache.h"
#include "xenos_ucode_key.h"
#include "smolv.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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
#include <rex/memory.h>

#include <fmt/format.h>

#include <cstdio>
#include <filesystem>
#include <string>

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

#include "shaders/rect_list.geom.inc"

// Where microcode without a recompiled shader is saved (SetPlumeMissDumpDirectory).
std::string g_miss_dump_directory;

struct UcodeMapEntry {
  uint64_t ucode;
  uint64_t container;
};
const UcodeMapEntry kUcodeMap[] = {
#include "shader_ucode_map.inc"
};

constexpr uint32_t kMaxTextures2D = 4096;
constexpr uint32_t kMaxTextures3D = 128;
constexpr uint32_t kMaxTexturesCube = 16;
constexpr uint32_t kMaxSamplers = 16;
constexpr uint64_t kUploadSize = 128ull << 20;
// Bindings 0..15 are the guest's vertex streams; this one feeds zeros to any
// input the draw has no data for.
constexpr uint32_t kZeroBinding = 16;
// Container hash of Direct3D's Clear vertex shader (microcode 1E6883FCCDE1F688,
// wrapped by tools/shaders/wrap_raw_ucode.py): position and colour of a
// rectangle from vertex fetch constant 0.
constexpr uint64_t kD3DClearVertexShader = 0xBA454DD8A2FCFCBAull;

enum Sampler : uint32_t {
  kSamplerLinearWrap,
  kSamplerLinearClamp,
  kSamplerLinearMirror,
  kSamplerPointWrap,
  kSamplerPointClamp,
  kSamplerPointMirror,
  kSamplerCount
};

uint32_t BSwap32(uint32_t v) { return __builtin_bswap32(v); }

// Copies guest data into host order, as the fetch constant's endian field says.
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = BSwap32(v);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
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
  std::vector<std::pair<uint32_t, bool>> inputs;  // vertex shaders only
  bool failed = false;
};

// A vertex element as the bound microcode fetches it.
struct LiveElement {
  uint32_t location;
  uint32_t slot;
  uint32_t offset, stride;
  uint32_t format;
  bool is_signed, normalized;
  uint32_t usage, usage_index;
};

class HostShader : public gfx::Shader {
 public:
  HostShader(xenos::ShaderType type, uint64_t hash, const uint32_t* ucode, size_t count)
      : Shader(type, hash, ucode, count) {}

  const ShaderCacheEntry* entry = nullptr;
  CompiledShader* compiled = nullptr;
  // Vertex shaders: the input layout decoded from this variant's vfetches.
  std::vector<LiveElement> elements;
  // Fetches the shader indexes with a computed register (vfetchLive): slot k
  // of the shared fetch table, as uint4 {stream | format << 8 | flags << 16,
  // stride, offset, exp scale bits}.
  std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>> live_fetches;
  bool elements_decoded = false;
};

class ShaderStore {
 public:
  ShaderStore() {
    for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
      by_container_.emplace(g_shaderCacheEntries[i].hash, &g_shaderCacheEntries[i]);
    }
    for (const auto& m : kUcodeMap) by_ucode_.emplace(m.ucode, m.container);
    REXLOG_INFO("Renut: Plume shader cache: {} shaders, {} microcode keys, {} MB smol-v",
                g_shaderCacheEntryCount, by_ucode_.size(),
                size_t(g_spirvCacheEnd - g_spirvCache) >> 20);
  }

  const ShaderCacheEntry* Find(uint64_t ucode_hash) const {
    auto container = by_ucode_.find(ucode_hash);
    if (container == by_ucode_.end()) return nullptr;
    auto entry = by_container_.find(container->second);
    return entry == by_container_.end() ? nullptr : entry->second;
  }

  CompiledShader* Build(RenderDevice* device, const ShaderCacheEntry* entry, bool vertex) {
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
    return &compiled;
  }

 private:
  std::unordered_map<uint64_t, const ShaderCacheEntry*> by_container_;
  std::unordered_map<uint64_t, uint64_t> by_ucode_;
  std::unordered_map<const ShaderCacheEntry*, CompiledShader> compiled_;
};

// Decodes the vertex layout a bound vertex shader really fetches. Direct3D
// patched the vfetch instructions for the current vertex declaration; the
// recompiler recorded which instruction reads each element (fetchAddress).
void DecodeLiveElements(HostShader& shader) {
  shader.elements_decoded = true;
  const uint32_t* u = shader.ucode_dwords();
  const size_t slots = shader.ucode_dword_count() / 3;
  // Fetch slots in program order, to find the full fetch a mini fetch follows.
  std::vector<uint8_t> be(shader.ucode_dword_count() * 4);
  for (size_t i = 0; i < shader.ucode_dword_count(); ++i) ucode::StoreBE(be.data() + i * 4, u[i]);
  const std::vector<uint32_t> fetch_slots = ucode::FetchSlots(be.data(), be.size());
  const ShaderInputElement* elements = g_shaderInputElements + shader.entry->inputElementOffset;
  for (uint32_t i = 0; i < shader.entry->inputElementCount; ++i) {
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
    if (live_fetch) {
      if (e.slot >= RENUT_LIVE_FETCH_COUNT) continue;
      const uint32_t flags = ((f[1] >> 12) & 1) | (((f[1] >> 13) & 1) << 1) | (((f[1] >> 14) & 1) << 2);
      const int32_t exp_adjust = int32_t(f[1] << 2) >> 26;  // bits 24..29, signed
      const float scale = std::ldexp(1.f, exp_adjust);
      uint32_t scale_bits;
      std::memcpy(&scale_bits, &scale, 4);
      shader.live_fetches.push_back(
          {e.slot, {(95 - fetch_constant) | (((f[1] >> 16) & 0x3F) << 8) | (flags << 16),
                    (full[2] & 0xFF) * 4, ((f[2] >> 8) & 0x7FFFFF) * 4, scale_bits}});
      continue;
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
    shader.elements.push_back(live);
  }
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
};

struct GuestTexture {
  std::unique_ptr<RenderTexture> texture;
  uint64_t checked_frame = ~0ull;
  uint32_t width = 0, height = 0;
  std::unique_ptr<RenderTextureView> view;  // cube maps
  uint32_t descriptor = 0;
  uint32_t levels = 1;
  uint64_t content = 0;
  bool failed = false;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
};

uint64_t ContentHash(const uint8_t* data, uint32_t span);

struct Stats {
  uint64_t textures_decoded = 0, texture_failures = 0;
  uint64_t draws = 0, drawn = 0, no_vs = 0, no_ps = 0, bad_shader = 0, skipped_prim = 0,
           no_target = 0, resolves = 0, clears = 0, pipelines = 0, overflow = 0;
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
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  // Direct3D's own Clear (see IssueDraw).
  void IssueD3DClear(xenos::EdramMode edram_mode);
  // Copies a rectangle of a depth target's depth into an R32_FLOAT texture.
  bool CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RenderTexture* dest,
                 uint32_t dest_x, uint32_t dest_y);
  std::unique_ptr<RenderBuffer> depth_staging_;

 private:
  // Upload ring (one frame; reset after every swap).
  uint64_t Allocate(uint64_t size, uint64_t alignment = 16);
  uint8_t* Pointer(uint64_t offset) { return upload_data_ + offset; }
  uint64_t Address(uint64_t offset) const { return upload_address_ + offset; }
  bool UploadGuest(uint32_t address, uint32_t size, xenos::Endian endian, uint64_t& offset);

  void BeginFrame();
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
  uint32_t GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestCubeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestStackedTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  void Report();

  RenderDevice* device_ = nullptr;
  RenderCommandQueue* queue_ = nullptr;
  std::unique_ptr<ShaderStore> store_;
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> shaders_;
  std::unordered_set<uint64_t> missed_;
  std::unordered_map<uint64_t, uint64_t> missed_draws_;
  std::unordered_set<uint64_t> missed_pairs_;
  std::string miss_dump_directory_;
  // One-frame trace: touching <files>/plume-trace-request records the next
  // frame's draws and resolves into <files>/plume-trace.txt.
  FILE* trace_ = nullptr;
  // Debug: <files>/plume-skip holds a bit mask of draw kinds to drop (see IssueDraw).
  uint32_t skip_mask_ = 0;
  // Debug: <files>/plume-drawskip holds "<first> <end>": draws of each frame
  // with an index in that range are dropped (to bisect a rendering bug).
  uint32_t drawskip_first_ = 0, drawskip_end_ = 0, frame_draw_ = 0;
  // Debug: <files>/plume-snap holds "<hex address> <n>": the n-th resolve of
  // the frame into that address is copied aside and presented instead of the
  // front buffer, to see an intermediate pass on screen.
  uint32_t snap_address_ = 0, snap_ordinal_ = 0, snap_seen_ = 0;
  std::unique_ptr<RenderTexture> snap_;
  uint32_t snap_width_ = 0, snap_height_ = 0;
  RenderFormat snap_format_ = RenderFormat::UNKNOWN;
  RenderTextureLayout snap_layout_ = RenderTextureLayout::UNKNOWN;
  bool snap_taken_ = false;
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
  std::array<std::unique_ptr<RenderSampler>, kSamplerCount> samplers_;
  std::unique_ptr<RenderTexture> white_2d_, white_3d_, white_cube_;
  std::unique_ptr<RenderTextureView> white_cube_view_;
  std::unique_ptr<RenderShader> rect_list_gs_;

  std::unique_ptr<RenderBuffer> upload_;
  uint8_t* upload_data_ = nullptr;
  uint64_t upload_address_ = 0;
  uint64_t upload_used_ = 0;
  uint64_t zero_offset_ = 0;
  std::unordered_map<uint64_t, uint64_t> uploaded_streams_;

  std::unique_ptr<RenderCommandList> list_;
  std::unique_ptr<RenderCommandFence> fence_;
  bool list_open_ = false;
  RenderFramebuffer* bound_framebuffer_ = nullptr;
  const RenderPipeline* bound_pipeline_ = nullptr;
  bool state_bound_ = false;

  std::unordered_map<uint64_t, Target> targets_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderFramebuffer>> framebuffers_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines_;
  // Keyed by guest address and host format: N&B reuses an address within a
  // frame for different things (a 768x768 shadow map, then the bloom chain),
  // and draws recorded earlier must keep the texture they sampled.
  std::unordered_map<uint64_t, Resolved> resolved_;
  // Textures replaced while the frame's commands may still use them; freed
  // once the frame has run (IssueSwap waits for it).
  std::vector<std::unique_ptr<RenderTexture>> retired_;
  // A recorded draw stores a bindless index, not a snapshot of that binding.
  // Keep old image bindings alive until the submission has finished as well.
  std::vector<uint32_t> retired_descriptors_, free_descriptors_;
  uint32_t AllocateTextureDescriptor() {
    if (!free_descriptors_.empty()) {
      uint32_t index = free_descriptors_.back();
      free_descriptors_.pop_back();
      return index;
    }
    return next_descriptor_ < kMaxTextures2D ? next_descriptor_++ : 0;
  }
  void RetireTexture(std::unique_ptr<RenderTexture>& texture, uint32_t& descriptor) {
    if (texture) retired_.push_back(std::move(texture));
    if (descriptor) retired_descriptors_.push_back(descriptor);
    descriptor = 0;
  }
  std::vector<std::unique_ptr<RenderFramebuffer>> retired_framebuffers_;
  static uint64_t ResolvedKey(uint32_t address, RenderFormat format) {
    return uint64_t(address) | (uint64_t(format) << 32);
  }
  // The texture a fetch at `address` sees: whichever resolve wrote there
  // last, as the memory would hold. The same address is a depth resolve in one
  // phase and a colour resolve in another (the depth-of-field pass also reads
  // the scene depth as 8_8_8_8), and a stale entry of the other kind must not
  // shadow the current one.
  Resolved* FindResolved(uint32_t address) {
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
            RetireTexture(it->second.texture, it->second.descriptor);
            RetireTexture(it->second.fitted, it->second.fitted_descriptor);
            it = resolved_.erase(it);
          } else {
            ++it;
          }
        }
        return nullptr;
      }
    }
    return latest;
  }
  uint64_t resolve_serial_ = 0;
  uint64_t target_serial_ = 0;
  std::unordered_map<uint64_t, GuestTexture> guest_textures_;
  std::unordered_set<uint32_t> unsupported_formats_;
  uint32_t next_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_cubes_;
  uint32_t next_cube_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_stacks_;
  uint32_t next_3d_descriptor_ = 1;

  Stats stats_;
  uint64_t swaps_ = 0;
};

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

  for (uint32_t i = 0; i < kSamplerCount; ++i) {
    RenderSamplerDesc desc;
    const bool point = i >= kSamplerPointWrap;
    desc.minFilter = desc.magFilter = point ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    desc.mipmapMode = point ? RenderMipmapMode::NEAREST : RenderMipmapMode::LINEAR;
    RenderTextureAddressMode mode = (i % 3) == 0   ? RenderTextureAddressMode::WRAP
                                    : (i % 3) == 1 ? RenderTextureAddressMode::CLAMP
                                                   : RenderTextureAddressMode::MIRROR;
    desc.addressU = desc.addressV = desc.addressW = mode;
    samplers_[i] = device_->createSampler(desc);
    set_samplers_->setSampler(i, samplers_[i].get());
  }

  upload_ = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      kUploadSize, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX |
                       RenderBufferFlag::DEVICE_ADDRESSABLE | RenderBufferFlag::STORAGE));
  if (!upload_) {
    REXLOG_ERROR("Renut: Plume upload buffer unavailable");
    return false;
  }
  upload_data_ = static_cast<uint8_t*>(upload_->map());
  upload_address_ = upload_->getDeviceAddress();

  list_ = queue_->createCommandList();
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

  upload_used_ = 0;
  uint64_t white = Allocate(4 * 6, 256);
  std::memset(Pointer(white), 0xFF, 4 * 6);
  zero_offset_ = Allocate(64, 16);
  std::memset(Pointer(zero_offset_), 0, 64);
  list_->begin();
  list_->barriers(RenderBarrierStage::COPY,
                  std::array{RenderTextureBarrier(white_2d_.get(), RenderTextureLayout::COPY_DEST),
                             RenderTextureBarrier(white_3d_.get(), RenderTextureLayout::COPY_DEST),
                             RenderTextureBarrier(white_cube_.get(), RenderTextureLayout::COPY_DEST)}
                      .data(),
                  3);
  auto footprint = RenderTextureCopyLocation::PlacedFootprint(upload_.get(), RenderFormat::R8G8B8A8_UNORM,
                                                              1, 1, 1, 64, white);
  list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_2d_.get()), footprint);
  list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_3d_.get()), footprint);
  for (uint32_t face = 0; face < 6; ++face) {
    list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_cube_.get(), 0, face), footprint);
  }
  list_->barriers(RenderBarrierStage::GRAPHICS,
                  std::array{RenderTextureBarrier(white_2d_.get(), RenderTextureLayout::SHADER_READ),
                             RenderTextureBarrier(white_3d_.get(), RenderTextureLayout::SHADER_READ),
                             RenderTextureBarrier(white_cube_.get(), RenderTextureLayout::SHADER_READ)}
                      .data(),
                  3);
  list_->end();
  const RenderCommandList* lists = list_.get();
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
  BeginFrame();
  REXLOG_INFO("Renut: Plume command processor ready (upload {} MB at {:X})", kUploadSize >> 20,
              upload_address_);
  return true;
}

void PlumeCommandProcessor::ShutdownContext() {
  if (list_open_) {
    list_->end();
    list_open_ = false;
  }
  if (queue_ && fence_) {
    // Nothing is in flight: every submission is awaited.
  }
  pipelines_.clear();
  framebuffers_.clear();
  targets_.clear();
  resolved_.clear();
  shaders_.clear();
  if (upload_) upload_->unmap();
  upload_.reset();
  CommandProcessor::ShutdownContext();
}

uint64_t PlumeCommandProcessor::Allocate(uint64_t size, uint64_t alignment) {
  uint64_t offset = (upload_used_ + alignment - 1) & ~(alignment - 1);
  if (offset + size > kUploadSize) {
    ++stats_.overflow;
    Flush();
    offset = (upload_used_ + alignment - 1) & ~(alignment - 1);
    if (offset + size > kUploadSize) return UINT64_MAX;
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
  uploaded_streams_.emplace(key, offset);
  return true;
}

void PlumeCommandProcessor::BeginFrame() {
  // Both callers wait for the GPU before starting a new command list.
  for (uint32_t index : retired_descriptors_) {
    set_2d_->setTexture(index, white_2d_.get(), RenderTextureLayout::SHADER_READ);
    free_descriptors_.push_back(index);
  }
  retired_descriptors_.clear();
  retired_framebuffers_.clear();
  retired_.clear();
  // Keep the permanent allocations (white texel, zeros) at the start.
  upload_used_ = zero_offset_ + 64;
  uploaded_streams_.clear();
  list_->begin();
  list_open_ = true;
  bound_framebuffer_ = nullptr;
  bound_pipeline_ = nullptr;
  state_bound_ = false;
}

// Runs what was recorded so far and waits, to make room in the upload ring.
void PlumeCommandProcessor::Flush() {
  if (!list_open_) return;
  list_->end();
  const RenderCommandList* lists = list_.get();
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  queue_->waitForCommandFence(fence_.get());
  BeginFrame();
}

void PlumeCommandProcessor::Transition(RenderTexture* texture, RenderTextureLayout& current,
                                       RenderTextureLayout layout) {
  if (current == layout) return;
  RenderBarrierStages stages = (layout == RenderTextureLayout::COPY_SOURCE ||
                                layout == RenderTextureLayout::COPY_DEST)
                                   ? RenderBarrierStage::COPY
                                   : RenderBarrierStage::GRAPHICS;
  list_->barriers(stages, RenderTextureBarrier(texture, layout));
  current = layout;
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
    list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
        RenderTextureCopyLocation::Subresource(tail.texture.get()), 0, y, 0, &box);
    dest.generation += tail.generation;
    RetireTexture(tail.texture, tail.descriptor);
    RetireTexture(tail.fitted, tail.fitted_descriptor);
    it = resolved_.erase(it);
  }
}

uint32_t PlumeCommandProcessor::FittedResolved(Resolved& r, uint32_t width, uint32_t height) {
  if (!r.fitted || r.fitted_width != width || r.fitted_height != height) {
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
    list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.fitted.get()),
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
  if (target.texture) retired_.push_back(std::move(target.texture));
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
  for (auto& [key, framebuffer] : framebuffers_) retired_framebuffers_.push_back(std::move(framebuffer));
  framebuffers_.clear();
  bound_framebuffer_ = nullptr;
  return &target;
}

RenderFramebuffer* PlumeCommandProcessor::GetFramebuffer(const std::array<Target*, 4>& colors,
                                                         uint32_t color_count, Target* depth) {
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
    list_->setGraphicsPipelineLayout(layout_.get());
    list_->setGraphicsDescriptorSet(set_2d_.get(), 0);
    list_->setGraphicsDescriptorSet(set_3d_.get(), 1);
    list_->setGraphicsDescriptorSet(set_cube_.get(), 2);
    list_->setGraphicsDescriptorSet(set_samplers_.get(), 3);
    state_bound_ = true;
  }
  if (bound_framebuffer_ != framebuffer) {
    list_->setFramebuffer(framebuffer);
    bound_framebuffer_ = framebuffer;
  }
}

Resolved& PlumeCommandProcessor::GetResolved(uint32_t address, RenderFormat format, uint32_t width,
                                             uint32_t height) {
  Resolved& resolved = resolved_[ResolvedKey(address, format)];
  if (resolved.texture && resolved.format == format && resolved.width >= width &&
      resolved.height >= height) {
    return resolved;
  }
  uint32_t w = std::max(width, resolved.format == format ? resolved.width : 0u);
  uint32_t h = std::max(height, resolved.format == format ? resolved.height : 0u);
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

uint32_t PlumeCommandProcessor::GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address) return 0;
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
  const uint32_t levels = last_level - top_level + 1;
  HashCombine(key, last_level);
  HashCombine(key, uint32_t(fetch.packed_mips));
  std::array<TextureLevel, 14> layouts;
  std::array<const uint8_t*, 14> data{};
  uint64_t content = 0;
  for (uint32_t i = 0; i < levels; ++i) {
    auto& level = layouts[i];
    if (!LocateTextureLevel(fetch, top_level + i, level)) return 0;
    const uint64_t start = (uint64_t(level.from_mips ? mip_page : base_page) << 12) + level.offset;
    if (!start || start + level.extent > 0x20000000ull) return 0;
    data[i] = memory_->TranslatePhysical<const uint8_t*>(uint32_t(start));
  }
  GuestTexture& texture = guest_textures_[key];
  if (texture.failed) return 0;
  // Checked once per frame; the draws after the first reuse the result.
  if (texture.texture && texture.checked_frame == swaps_ && texdump_directory_.empty()) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
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
  if (!texture.descriptor) return 0;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  uint32_t w = width, h = height;
  for (uint32_t level = 0; level < levels; ++level) {
    if (level && !DecodeTextureLevel(fetch, layouts[level], data[level], 0, rgba)) return 0;
    w = layouts[level].width;
    h = layouts[level].height;
    const uint64_t staging = Allocate(uint64_t(w) * h * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), rgba.data(), size_t(w) * h * 4);
    list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), level),
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

// Stacked textures (arrays of 2D slices: the terrain's material and normal map
// sets), which the shaders sample with tfetch3D, z picking the slice. Slices
// follow each other at a 4 KB aligned stride; bound as a 3D texture of that
// depth, one level (mips would halve the slice count too).
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
  if (skip_mask_ & 32768) { base_page = fetch.base_address; top_level = 0; }
  TextureLevel level;
  if (!LocateTextureLevel(fetch, top_level, level)) return 0;
  const uint64_t start = (uint64_t(level.from_mips ? mip_page : base_page) << 12) + level.offset;
  if (!start || start + level.extent > 0x20000000ull) return 0;
  const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(uint32_t(start));
  GuestTexture& texture = guest_stacks_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_ && texdump_directory_.empty()) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  const uint64_t content = ContentHash(data, level.extent);
  if (!texdump_directory_.empty() && dumped_.insert(key ^ 2).second) texture.content = ~content;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> all, slice;
  uint32_t width = 0, height = 0;
  for (uint32_t z = 0; z < depth; ++z) {
    const uint32_t w = level.width, h = level.height;
    if (!DecodeTextureLevel(fetch, level, data, z, slice)) {
      texture.failed = true;
      ++stats_.texture_failures;
      return 0;
    }
    width = w;
    height = h;
    if (!texdump_directory_.empty()) DumpTexture(fetch, slice, w, h, "stack");
    all.insert(all.end(), slice.begin(), slice.end());
  }
  if (!texture.descriptor) {
    if (next_3d_descriptor_ >= kMaxTextures3D) return 0;
    texture.descriptor = next_3d_descriptor_++;
  }
  if (!texture.texture || texture.width != width || texture.height != height || texture.levels != depth) {
    if (texture.texture) retired_.push_back(std::move(texture.texture));
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture3D(width, height, depth, 1, RenderFormat::R8G8B8A8_UNORM));
    texture.width = width;
    texture.height = height;
    texture.levels = depth;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
  }
  texture.content = content;
  const uint64_t staging = Allocate(uint64_t(width) * height * depth * 4, 512);
  if (staging == UINT64_MAX) return 0;
  std::memcpy(Pointer(staging), all.data(), all.size() * 4);
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get()),
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
    if (next_cube_descriptor_ >= kMaxTexturesCube) return 0;
    texture.descriptor = next_cube_descriptor_++;
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
    list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), 0, face),
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

gfx::Shader* PlumeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t,
                                               const uint32_t* host_address, uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, size_t(dword_count) * sizeof(uint32_t));
  auto& shader = shaders_[hash];
  if (!shader) {
    shader = std::make_unique<HostShader>(shader_type, hash, host_address, dword_count);
    shader->entry = store_->Find(ucode::Key(host_address, size_t(dword_count) * sizeof(uint32_t)));
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
  return shader.get();
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
  // Direct3D's Clear draws a rectangle with its own shader, in an EDRAM view
  // of the targets (half the pitch at 64 bpp, all four colour targets, a
  // 4096-row surface) that only aliases the real ones on the hardware. Drawn
  // literally it would clear render targets of its own; clear the real ones.
  if (vs->entry->hash == kD3DClearVertexShader && prim_type == xenos::PrimitiveType::kRectangleList &&
      !(skip_mask_ & 2048)) {
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
  if (!vs->compiled) vs->compiled = store_->Build(device_, vs->entry, true);
  if (ps && !ps->compiled) ps->compiled = store_->Build(device_, ps->entry, false);
  if (!vs->compiled || (ps && !ps->compiled)) {
    ++stats_.bad_shader;
    return true;
  }
  if (!vs->elements_decoded) DecodeLiveElements(*vs);

  // Primitive.
  RenderPrimitiveTopology topology;
  bool rect_list = false, quad_list = false;
  switch (prim_type) {
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
    default:
      ++stats_.skipped_prim;
      Trace("dropped prim={} n={}", uint32_t(prim_type), index_count);
      return true;
  }
  if (rect_list && !rect_list_gs_) {
    ++stats_.skipped_prim;
    return true;
  }

  // Render targets.
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t pitch = surface.surface_pitch;
  if (!pitch) return true;
  const auto window_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  const uint32_t height = std::clamp<uint32_t>(window_br.br_y, 16, 4096);
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
  if (!color_count && !depth) return true;
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
  for (uint32_t slot = 0; slot < 32; ++slot) {
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
    if (fetch.type != xenos::FetchConstantType::kTexture) continue;
    bool point = fetch.min_filter == xenos::TextureFilter::kPoint;
    uint32_t mode = fetch.clamp_x == xenos::ClampMode::kRepeat           ? 0
                    : fetch.clamp_x == xenos::ClampMode::kMirroredRepeat ? 2
                                                                         : 1;
    sampler_descriptor[slot] = (point ? kSamplerPointWrap : kSamplerLinearWrap) + mode;
    Resolved* found = FindResolved(fetch.base_address << 12);
    if (found && found->descriptor) {
      Resolved& r = *found;
      const uint32_t fw = fetch.size_2d.width + 1, fh = fetch.size_2d.height + 1;
      if (fw < r.width || fh < r.height) {
        texture_descriptor[slot] = FittedResolved(r, std::min(fw, r.width), std::min(fh, r.height));
      } else {
        Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
        texture_descriptor[slot] = r.descriptor;
      }
    } else if (fetch.dimension == xenos::DataDimension::k2DOrStacked) {
      texture_descriptor[slot] = GetGuestTexture(fetch);
      if (fetch.stacked) stack_descriptor[slot] = GetGuestStackedTexture(fetch);
    } else if (fetch.dimension == xenos::DataDimension::kCube) {
      cube_descriptor[slot] = GetGuestCubeTexture(fetch);
    }
  }

  if (trace_) {
    std::string line = fmt::format("draw #{} vs={:016X} ps={:016X} prim={} n={} mode={} pitch={} h={} cc={:X}",
                                   draw_index,
                                   vs->entry->hash, ps ? ps->entry->hash : 0, uint32_t(prim_type),
                                   index_count, uint32_t(edram_mode), pitch, height,
                                   regs[XE_GPU_REG_RB_COLORCONTROL]);
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
      line += fmt::format(" t{}={:08X}/f{}/{}x{}/d{}{}", slot, fetch.base_address << 12,
                          uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                          uint32_t(fetch.dimension),
                          FindResolved(fetch.base_address << 12) ? "/R" : "");
    }
    Trace("{}", line);
    Trace("  raster mode={:08X} offset=({},{},{},{}) blend={:08X} constants=({},{},{},{}) alpha={}",
          regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET), regs[XE_GPU_REG_RB_BLENDCONTROL0],
          regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
          regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
          regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF));
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
  uint64_t vs_constants = Allocate(4096, 256);
  uint64_t ps_constants = Allocate(4096, 256);
  uint64_t shared = Allocate(RENUT_SHARED_SIZE, 256);
  if (vs_constants == UINT64_MAX || ps_constants == UINT64_MAX || shared == UINT64_MAX) return true;
  std::memcpy(Pointer(vs_constants), &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], 4096);
  std::memcpy(Pointer(ps_constants), &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X], 4096);
  uint8_t* sh = Pointer(shared);
  std::memset(sh, 0, RENUT_SHARED_SIZE);
  std::memcpy(sh + RENUT_SHARED_TEXTURE2D_OFFSET, texture_descriptor, sizeof(texture_descriptor));
  std::memcpy(sh + RENUT_SHARED_TEXTURECUBE_OFFSET, cube_descriptor, sizeof(cube_descriptor));
  std::memcpy(sh + RENUT_SHARED_TEXTURE3D_OFFSET, stack_descriptor, sizeof(stack_descriptor));
  std::memcpy(sh + RENUT_SHARED_SAMPLER_OFFSET, sampler_descriptor, sizeof(sampler_descriptor));
  std::memcpy(sh + RENUT_SHARED_BOOLEANS_OFFSET, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              RENUT_BOOL_REGISTER_COUNT / 8);
  for (uint32_t i = 0; i < RENUT_LOOP_CONSTANT_COUNT; ++i) {
    uint32_t v = regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
    int32_t loop[4] = {int32_t(v & 0xFF), int32_t((v >> 8) & 0xFF), int32_t(int8_t((v >> 16) & 0xFF)), 0};
    std::memcpy(sh + RENUT_SHARED_LOOP_OFFSET + i * 16, loop, 16);
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const bool alpha_test_enabled = color_control.alpha_test_enable &&
                                  color_control.alpha_func != xenos::CompareFunction::kAlways;
  // Alpha to coverage (N&B's grass and foliage) has no samples to cover
  // without MSAA; like a single-sample A2C, keep the fragments at or above
  // half coverage.
  const bool alpha_to_mask = color_control.alpha_to_mask_enable && !(skip_mask_ & 4096);
  const bool alpha_test = ps && (alpha_test_enabled || alpha_to_mask);
  float alpha_ref = alpha_test_enabled ? regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF) : 0.5f;
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 12, &alpha_ref, 4);

  // Vertex inputs: the layout the bound vertex shader's vfetches describe
  // (DecodeLiveElements); the streams they name are the guest's vertex fetch
  // constants 95 - stream.
  std::vector<RenderInputElement> input_elements;
  std::vector<RenderInputSlot> input_slots;
  std::array<uint64_t, 17> slot_offsets{};
  std::array<uint32_t, 17> slot_sizes{};
  std::array<bool, 17> slot_used{};
  uint32_t swapped[7] = {};  // texcoord, normal, binormal, tangent, blendweight, position, blendindices
  uint32_t packed_inputs = 0, int_inputs = 0;
  bool r11g11b10_normals = false;
  uint64_t streams[RENUT_VERTEX_STREAM_COUNT] = {};
  for (uint32_t stream = 0; stream < RENUT_VERTEX_STREAM_COUNT; ++stream) {
    // The last entry is fetch constant 0, which Direct3D's own Clear uses.
    xenos::xe_gpu_vertex_fetch_t fetch =
        regs.GetVertexFetch(stream == RENUT_DIRECT_FETCH_STREAM ? 0 : 95 - stream);
    if (fetch.type != xenos::FetchConstantType::kVertex) continue;
    uint64_t offset;
    if (UploadGuest(fetch.address << 2, fetch.size << 2, fetch.endian, offset)) {
      streams[stream] = Address(offset);
      slot_offsets[stream] = offset;
      slot_sizes[stream] = fetch.size << 2;
    }
  }
  std::memcpy(sh + RENUT_SHARED_STREAMS_OFFSET, streams, sizeof(streams));
  for (const auto& [k, entry] : vs->live_fetches) {
    std::memcpy(sh + RENUT_SHARED_FETCH_OFFSET + k * 16, entry.data(), 16);
  }
  bool zero_used = false;
  for (auto [location, integer] : vs->compiled->inputs) {
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
    // 16-bit elements need no swapFloats(): after the k8in32 swap the input
    // assembler reads the hardware's component order (X in the low half), and
    // Direct3D already patched the vfetch swizzle (.yxwz) to put the halves
    // back in memory order. Swapping here as well mangled every mesh with
    // half-float positions; only the manually fetched grass survived.
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
  // g_SwappedTexcoords +0, Normals +16, Binormals +20, Tangents +24,
  // BlendWeights +28, Positions +32.
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 0, &swapped[0], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 16, &swapped[1], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 20, &swapped[2], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 24, &swapped[3], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 28, &swapped[4], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 32, &swapped[5], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 40, &swapped[6], 4);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 44, &packed_inputs, 4);
  std::memcpy(sh + RENUT_SHARED_INPUTS_OFFSET, &int_inputs, 4);

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
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
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
  std::memcpy(sh + RENUT_SHARED_VIEWPORT_OFFSET, scale_offset, sizeof(scale_offset));
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  RenderRect scissor_rect(int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
                          int32_t(scissor.offset[0] + scissor.extent[0]),
                          int32_t(scissor.offset[1] + scissor.extent[1]));
  if (trace_) {
    Trace("  viewport ({}, {}) {}x{} window ({}, {}) scissor ({}, {})-({}, {}) vte={:X}", viewport.x,
          viewport.y, viewport.width, viewport.height, window_x, window_y, scissor_rect.left,
          scissor_rect.top, scissor_rect.right, scissor_rect.bottom, vte.value);
    Trace("  zviewport scale={} offset={}", regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
  }
  if (scissor_rect.right <= scissor_rect.left || scissor_rect.bottom <= scissor_rect.top) return true;

  // Pipeline.
  const auto raster = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const bool polygonal = draw_util::IsPrimitivePolygonal(false, prim_type);
  RenderCullMode cull = RenderCullMode::NONE;
  if (polygonal && !(skip_mask_ & 131072)) {
    if (raster.cull_front && raster.cull_back) return true;
    if (raster.cull_front) cull = RenderCullMode::FRONT;
    else if (raster.cull_back) cull = RenderCullMode::BACK;
  }
  const RenderFrontFace front_face = raster.face ? RenderFrontFace::CLOCKWISE
                                                : RenderFrontFace::COUNTER_CLOCKWISE;
  const bool depth_enabled = depth && depth_control.z_enable && !(skip_mask_ & 1024);
  const bool depth_write = depth_enabled && depth_control.z_write_enable;
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
  }
  const uint32_t spec = (alpha_test ? SPEC_CONSTANT_ALPHA_TEST : 0) |
                        (r11g11b10_normals ? SPEC_CONSTANT_R11G11B10_NORMAL : 0);
  uint64_t key = 0;
  HashCombine(key, vs->compiled);
  HashCombine(key, ps ? ps->compiled : nullptr);
  HashCombine(key, topology);
  HashCombine(key, rect_list);
  HashCombine(key, cull);
  HashCombine(key, front_face);
  HashCombine(key, color_count);
  for (uint32_t i = 0; i < color_count; ++i) {
    HashCombine(key, colors[i]->format);
    HashCombine(key, blends[i].blendEnabled);
    HashCombine(key, blends[i].srcBlend);
    HashCombine(key, blends[i].dstBlend);
    HashCombine(key, blends[i].blendOp);
    HashCombine(key, blends[i].srcBlendAlpha);
    HashCombine(key, blends[i].dstBlendAlpha);
    HashCombine(key, blends[i].blendOpAlpha);
    HashCombine(key, blends[i].renderTargetWriteMask);
  }
  HashCombine(key, depth != nullptr);
  HashCombine(key, depth_enabled);
  HashCombine(key, depth_write);
  HashCombine(key, depth_control.zfunc);
  HashCombine(key, spec);
  for (const auto& e : input_elements) {
    HashCombine(key, e.location);
    HashCombine(key, e.format);
    HashCombine(key, e.slotIndex);
    HashCombine(key, e.alignedByteOffset);
  }
  for (const auto& s : input_slots) {
    HashCombine(key, s.index);
    HashCombine(key, s.stride);
  }
  auto& pipeline = pipelines_[key];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = vs->compiled->module.get();
    desc.geometryShader = rect_list ? rect_list_gs_.get() : nullptr;
    desc.pixelShader = ps ? ps->compiled->module.get() : nullptr;
    desc.primitiveTopology = topology;
    desc.cullMode = cull;
    desc.frontFace = front_face;
    desc.depthClipEnabled = true;
    desc.depthEnabled = depth_enabled;
    desc.depthWriteEnabled = depth_write;
    desc.depthFunction = depth_enabled ? Compare(depth_control.zfunc) : RenderComparisonFunction::ALWAYS;
    desc.depthTargetFormat = depth ? depth->format : RenderFormat::UNKNOWN;
    desc.renderTargetCount = color_count;
    for (uint32_t i = 0; i < color_count; ++i) {
      desc.renderTargetFormat[i] = colors[i]->format;
      desc.renderTargetBlend[i] = blends[i];
    }
    desc.inputElements = input_elements.data();
    desc.inputElementsCount = uint32_t(input_elements.size());
    desc.inputSlots = input_slots.data();
    desc.inputSlotsCount = uint32_t(input_slots.size());
    RenderSpecConstant spec_constant(0, spec);
    desc.specConstants = &spec_constant;
    desc.specConstantsCount = 1;
    pipeline = device_->createGraphicsPipeline(desc);
    ++stats_.pipelines;
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
  uint32_t index_bytes = 0;
  if (indexed) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    index_format = is32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT;
    const uint32_t size = index_count * (is32 ? 4 : 2);
    if (!UploadGuest(index_buffer_info->guest_base, size, index_buffer_info->endianness, index_offset)) {
      return true;
    }
    index_bytes = size;
  }
  if (quad_list) {
    // Two triangles per quad, as 32-bit indices.
    const uint32_t quads = index_count / 4;
    uint64_t offset = Allocate(uint64_t(quads) * 6 * 4, 256);
    if (offset == UINT64_MAX) return true;
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
    index_offset = offset;
    index_bytes = quads * 6 * 4;
    vertex_count = quads * 6;
  }

  // Record.
  RenderFramebuffer* framebuffer = GetFramebuffer(colors, color_count, depth);
  if (!framebuffer) {
    ++stats_.no_target;
    return true;
  }
  SetFramebuffer(framebuffer);
  if (bound_pipeline_ != pipeline.get()) {
    list_->setPipeline(pipeline.get());
    bound_pipeline_ = pipeline.get();
  }
  const uint64_t push[3] = {Address(vs_constants), Address(ps_constants), Address(shared)};
  list_->setGraphicsPushConstants(0, push, 0, sizeof(push));
  list_->setViewports(&viewport, 1);
  list_->setScissors(&scissor_rect, 1);
  for (const auto& slot : input_slots) {
    RenderVertexBufferView view;
    if (slot.index == kZeroBinding) {
      view = RenderVertexBufferView(RenderBufferReference(upload_.get(), zero_offset_), 64);
    } else {
      view = RenderVertexBufferView(RenderBufferReference(upload_.get(), slot_offsets[slot.index]),
                                    slot_sizes[slot.index]);
    }
    list_->setVertexBuffers(slot.index, &view, 1, &slot);
  }
  const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
  if (trace_) {
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
      if (e.usage != 0 || !e.stride || !slot_sizes[e.slot]) continue;
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
  if (indexed) {
    RenderIndexBufferView view(RenderBufferReference(upload_.get(), index_offset), index_bytes,
                               index_format);
    list_->setIndexBuffer(&view);
    list_->drawIndexedInstanced(vertex_count, 1, 0, base_vertex, 0);
  } else {
    list_->drawInstanced(vertex_count, 1, uint32_t(std::max(base_vertex, 0)), 0);
  }
  ++stats_.drawn;
  return true;
}

bool PlumeCommandProcessor::IssueCopy() {
  const gfx::RegisterFile& regs = *register_file_;
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) return true;
  ++stats_.resolves;
  const uint32_t width = info.coordinate_info.width_div_8 * 8;
  const uint32_t height = info.height_div_8 * 8;
  if (!width || !height) return true;

  const bool copying_depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram = copying_depth ? info.depth_edram_info : info.color_edram_info;
  const uint32_t original = copying_depth ? info.depth_original_base : info.color_original_base;
  // Source rectangle in pixels: whole tiles from the render target's base,
  // plus the offset within the first tile.
  const uint32_t tile_width = edram.format_is_64bpp ? 40 : 80;
  // EDRAM bases are shared between targets of different pitches (both shadow
  // maps use depth base 0), so prefer the one the resolve's pitch names.
  Target* source = FindTarget(original, edram.format, copying_depth,
                              std::max(1u, uint32_t(edram.pitch_tiles)) * tile_width);
  const uint32_t tiles = edram.base_tiles - original;
  const uint32_t pitch_tiles = std::max(1u, uint32_t(edram.pitch_tiles));
  const uint32_t x = (tiles % pitch_tiles) * tile_width + info.coordinate_info.edram_offset_x_div_8 * 8;
  const uint32_t y = (tiles / pitch_tiles) * 16 + info.coordinate_info.edram_offset_y_div_8 * 8;

  Trace("resolve {} base={:X} fmt={} src=({},{}) {}x{} found={} -> {:08X} dfmt={} ({},{}) pitch={} h={} clear={}{}",
        copying_depth ? "depth" : "color", original, uint32_t(edram.format), x, y, width, height,
        source != nullptr, uint32_t(info.copy_dest_base), uint32_t(info.copy_dest_info.copy_dest_format),
        info.copy_dest_coordinate_info.offset_x_div_8 * 8, info.copy_dest_coordinate_info.offset_y_div_8 * 8,
        info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
        info.copy_dest_coordinate_info.height_aligned_div_32 * 32, info.IsClearingColor() ? "c" : "",
        info.IsClearingDepth() ? "d" : "");
  if (source && x < source->width && y < source->height &&
      info.rb_copy_control.copy_command != xenos::CopyCommand::kNull) {
    const uint32_t w = std::min(width, source->width - x), h = std::min(height, source->height - y);
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
    // depth the depth-of-field and fog passes read).
    const RenderFormat dest_format = copying_depth ? RenderFormat::R32_FLOAT : source->format;
    Resolved* inside = FindResolvedContaining(info.copy_dest_base, dest_format, dest_bpp,
                                              dest_w, dest_h, column, row);
    Resolved& dest = inside && inside->width >= column + dx + w && inside->height >= row + dy + h
                         ? *inside
                         : GetResolved(info.copy_dest_base, dest_format, dest_w + dx, dest_h + dy);
    const uint32_t dest_y = &dest == inside ? dy + row : dy;
    const uint32_t dest_x = &dest == inside ? dx + column : dx;
    ++dest.generation;
    dest.written = ++resolve_serial_;
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
      CopyDepth(*source, x, y, w, h, dest.texture.get(), dest_x, dest_y);
    } else if (dest.texture) {
      Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h), 0, 1);
      list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
                               RenderTextureCopyLocation::Subresource(source->texture.get()), dest_x,
                               dest_y, 0, &box);
    }
    if (dest.texture) {
      if (snap_address_ && info.copy_dest_base == snap_address_ && ++snap_seen_ == snap_ordinal_) {
        if (!snap_ || snap_width_ != dest.width || snap_height_ != dest.height || snap_format_ != dest.format) {
          snap_ = device_->createTexture(
              RenderTextureDesc::Texture2D(dest.width, dest.height, 1, dest.format));
          snap_width_ = dest.width;
          snap_height_ = dest.height;
          snap_format_ = dest.format;
          snap_layout_ = RenderTextureLayout::UNKNOWN;
        }
        if (snap_) {
          Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_SOURCE);
          Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_DEST);
          list_->copyTexture(snap_.get(), dest.texture.get());
          snap_taken_ = true;
        }
      }
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
      if (dest.descriptor) {
        set_2d_->setTexture(dest.descriptor, dest.texture.get(), RenderTextureLayout::SHADER_READ);
      }
    }
  }

  // Clears that ride along with the resolve.
  const RenderRect rect(int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height));
  if (info.IsClearingColor() && source) {
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
    list_->clearColor(0, color, &rect, 1);
    ++stats_.clears;
  }
  if (info.IsClearingDepth()) {
    Target* depth = FindTarget(info.depth_original_base, info.depth_edram_info.format, true,
                               std::max(1u, uint32_t(info.depth_edram_info.pitch_tiles)) * 80);
    if (depth) {
      Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      SetFramebuffer(GetFramebuffer(none, 0, depth));
      const uint32_t d = info.rb_depth_clear;
      list_->clearDepthStencil(true, true, float(d >> 8) / float(0xFFFFFF), d & 0xFF, &rect, 1);
      ++stats_.clears;
    }
  }
  return true;
}

bool PlumeCommandProcessor::CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                      RenderTexture* dest, uint32_t dest_x, uint32_t dest_y) {
  // Vulkan copies a depth aspect only to a buffer, so go through one: depth
  // to buffer (4 bytes per texel for D32_SFLOAT), buffer to the colour image.
  constexpr uint64_t kStagingSize = 4096ull * 2048 * 4;
  if (uint64_t(w) * h * 4 > kStagingSize) return false;
  if (!depth_staging_) {
    depth_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize));
    if (!depth_staging_) return false;
  }
  // Always a barrier call, which also ends any open render pass before the
  // raw Vulkan commands below.
  list_->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(source.texture.get(), RenderTextureLayout::COPY_SOURCE));
  source.layout = RenderTextureLayout::COPY_SOURCE;
  bound_framebuffer_ = nullptr;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(list_.get())->vk;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {int32_t(x), int32_t(y), 0};
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cb, static_cast<VulkanTexture*>(source.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<VulkanBuffer*>(depth_staging_.get())->vk, 1, &region);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                           RenderTextureCopyLocation::PlacedFootprint(depth_staging_.get(),
                                                                      RenderFormat::R32_FLOAT, w, h, 1, w),
                           dest_x, dest_y);
  return true;
}

void PlumeCommandProcessor::IssueD3DClear(xenos::EdramMode edram_mode) {
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
  const uint32_t clear_height = std::clamp<uint32_t>(
      regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y, 16, 4096);
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
      list_->clearColor(0, RenderColor(v[3], v[4], v[5], v[6]), &rect, 1);
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
    Trace("  clear depth base={:X} -> {}", uint32_t(depth_info.depth_base),
          depth ? fmt::format("target {}x{}", depth->width, depth->height) : std::string("none"));
    if (depth) {
      Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      SetFramebuffer(GetFramebuffer(none, 0, depth));
      const RenderRect rect(0, 0, int32_t(depth->width), int32_t(depth->height));
      list_->clearDepthStencil(clear_depth, clear_stencil, std::clamp(v[2], 0.f, 1.f),
                               regs.Get<reg::RB_STENCILREFMASK>().stencilref, &rect, 1);
    }
  }
}

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  auto* renderer = PlumeRenderer::Get();
  RenderTexture* source = nullptr;
  Resolved* found = FindResolved(frontbuffer_ptr);
  if (found && found->texture) {
    Resolved& r = *found;
    Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
    source = r.texture.get();
    frontbuffer_width = std::min(frontbuffer_width, r.width);
    frontbuffer_height = std::min(frontbuffer_height, r.height);
  }
  if (snap_taken_ && snap_) {
    Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_SOURCE);
    source = snap_.get();
    frontbuffer_width = std::min(frontbuffer_width, snap_width_);
    frontbuffer_height = std::min(frontbuffer_height, snap_height_);
  }
  snap_taken_ = false;
  snap_seen_ = 0;
  frame_draw_ = 0;
  list_->end();
  list_open_ = false;
  if (renderer) {
    renderer->SubmitAndPresent(list_.get(), source, frontbuffer_width, frontbuffer_height);
  }
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
    std::fclose(trace_);
    trace_ = nullptr;
    REXLOG_INFO("Renut: Plume frame trace written");
  }
  if (++swaps_ % 30 == 0 && !miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    const std::string request = root + "/plume-trace-request";
    if (std::remove(request.c_str()) == 0) trace_ = std::fopen((root + "/plume-trace.txt").c_str(), "w");
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
    if (FILE* file = std::fopen((root + "/plume-skip").c_str(), "r")) {
      if (std::fscanf(file, "%u", &mask) != 1) mask = 0;
      std::fclose(file);
    }
    if (mask != skip_mask_) REXLOG_INFO("Renut: Plume skip mask {:X}", mask);
    skip_mask_ = mask;
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
  }
  if (swaps_ % 300 == 1) Report();
  BeginFrame();
}

void PlumeCommandProcessor::Report() {
  REXLOG_INFO(
      "Renut: Plume frame {}: draws={} drawn={} no_vs={} no_ps={} bad_shader={} skipped_prim={} "
      "no_target={} resolves={} clears={} pipelines={} targets={} resolved={} shaders={} "
      "missing_ucode={} upload_overflow={} upload_used={}KB textures={} decoded={} "
      "texture_failures={}",
      swaps_, stats_.draws, stats_.drawn, stats_.no_vs, stats_.no_ps, stats_.bad_shader,
      stats_.skipped_prim, stats_.no_target, stats_.resolves, stats_.clears, stats_.pipelines,
      targets_.size(), resolved_.size(), shaders_.size(), missed_.size(), stats_.overflow,
      upload_used_ >> 10, guest_textures_.size(), stats_.textures_decoded,
      stats_.texture_failures);
  for (const auto& [hash, count] : missed_draws_) {
    REXLOG_INFO("Renut: Plume   missing VS {:016X}: {} draws", hash, count);
  }
  missed_draws_.clear();
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
