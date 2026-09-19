#include "plume_gpu.h"

#include "plume_renderer.h"
#include "plume_textures.h"
#include "plume_vulkan.h"
#include "shader_cache.h"
#include "xenos_ucode_key.h"
#include "smolv.h"

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
constexpr uint32_t kMaxTextures3D = 16;
constexpr uint32_t kMaxTexturesCube = 16;
constexpr uint32_t kMaxSamplers = 16;
constexpr uint64_t kUploadSize = 128ull << 20;
// Bindings 0..15 are the guest's vertex streams; this one feeds zeros to any
// input the draw has no data for.
constexpr uint32_t kZeroBinding = 16;

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

RenderFormat VertexFormat(uint32_t format, bool is_signed, bool normalized, bool integer_input) {
  if (integer_input) {
    switch (format) {
      case 6: return RenderFormat::R8G8B8A8_UINT;
      case 25: return RenderFormat::R16G16_UINT;
      case 26: return RenderFormat::R16G16B16A16_UINT;
      default: return RenderFormat::R32G32B32A32_UINT;
    }
  }
  switch (format) {
    case 6:
      return normalized ? (is_signed ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM)
                        : (is_signed ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT);
    case 25:
      return normalized ? (is_signed ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM)
                        : (is_signed ? RenderFormat::R16G16_SINT : RenderFormat::R16G16_UINT);
    case 26:
      return normalized
                 ? (is_signed ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM)
                 : (is_signed ? RenderFormat::R16G16B16A16_SINT : RenderFormat::R16G16B16A16_UINT);
    case 31: return RenderFormat::R16G16_FLOAT;
    case 32: return RenderFormat::R16G16B16A16_FLOAT;
    case 33: return is_signed ? RenderFormat::R32_SINT : RenderFormat::R32_UINT;
    case 34: return is_signed ? RenderFormat::R32G32_SINT : RenderFormat::R32G32_UINT;
    case 35: return is_signed ? RenderFormat::R32G32B32A32_SINT : RenderFormat::R32G32B32A32_UINT;
    case 36: return RenderFormat::R32_FLOAT;
    case 37: return RenderFormat::R32G32_FLOAT;
    case 38: return RenderFormat::R32G32B32A32_FLOAT;
    case 57: return RenderFormat::R32G32B32_FLOAT;
    case 7:
    case 16:
    case 17: return RenderFormat::R32_UINT;
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
};

struct Resolved {
  std::unique_ptr<RenderTexture> texture;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t descriptor = 0;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
};

struct GuestTexture {
  std::unique_ptr<RenderTexture> texture;
  uint32_t descriptor = 0;
  uint64_t content = 0;
  bool failed = false;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
};

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
  Target* FindTarget(uint32_t base, uint32_t format, bool depth);
  RenderFramebuffer* GetFramebuffer(const std::array<Target*, 4>& colors, uint32_t color_count,
                                    Target* depth);
  void SetFramebuffer(RenderFramebuffer* framebuffer);
  Resolved& GetResolved(uint32_t address, RenderFormat format, uint32_t width, uint32_t height);
  uint32_t GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  void Report();

  RenderDevice* device_ = nullptr;
  RenderCommandQueue* queue_ = nullptr;
  std::unique_ptr<ShaderStore> store_;
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> shaders_;
  std::unordered_set<uint64_t> missed_;
  std::unordered_map<uint64_t, uint64_t> missed_draws_;
  std::unordered_set<uint64_t> missed_pairs_;
  std::string miss_dump_directory_;

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
  std::unordered_map<uint32_t, Resolved> resolved_;
  std::unordered_map<uint64_t, GuestTexture> guest_textures_;
  std::unordered_set<uint32_t> unsupported_formats_;
  uint32_t next_descriptor_ = 1;

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
  if (found != uploaded_streams_.end()) {
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

Target* PlumeCommandProcessor::FindTarget(uint32_t base, uint32_t format, bool depth) {
  Target* best = nullptr;
  for (auto& [key, target] : targets_) {
    if ((key & 0xFFF) == base && ((key >> 12) & 0xF) == format && bool((key >> 16) & 1) == depth) {
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
  if (target.texture && target.height >= height) return &target;
  // New, or taller than before: (re)create. Contents are lost on growth.
  uint32_t new_height = std::max({height, target.height, 64u});
  RenderFormat rf = depth ? RenderFormat::D32_FLOAT_S8_UINT
                          : ColorFormat(xenos::ColorRenderTargetFormat(format));
  RenderTextureDesc desc = depth ? RenderTextureDesc::DepthTarget(pitch, new_height, rf)
                                 : RenderTextureDesc::ColorTarget(pitch, new_height, rf);
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
  Resolved& resolved = resolved_[address];
  if (resolved.texture && resolved.format == format && resolved.width >= width &&
      resolved.height >= height) {
    return resolved;
  }
  uint32_t w = std::max(width, resolved.format == format ? resolved.width : 0u);
  uint32_t h = std::max(height, resolved.format == format ? resolved.height : 0u);
  resolved.texture = device_->createTexture(RenderTextureDesc::Texture2D(w, h, 1, format));
  resolved.format = format;
  resolved.width = w;
  resolved.height = h;
  resolved.layout = RenderTextureLayout::UNKNOWN;
  if (!resolved.descriptor && next_descriptor_ < kMaxTextures2D) {
    resolved.descriptor = next_descriptor_++;
  }
  return resolved;
}

// The game's own textures: the base level, decoded on the CPU into RGBA8 the
// first time a fetch constant names it (and again if its bytes change).
uint32_t PlumeCommandProcessor::GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address) return 0;
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);  // pitch, tiling, (not clamp/sign)
  HashCombine(key, fetch.dword_1);            // format, endian, base address
  HashCombine(key, fetch.dword_2);            // size
  HashCombine(key, fetch.swizzle);
  const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(address);
  const uint32_t span = TextureBaseSpan(fetch);
  const uint64_t content = XXH3_64bits(data, std::min<uint32_t>(span, 1024));
  GuestTexture& texture = guest_textures_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> rgba;
  uint32_t width = 0, height = 0;
  if (!span || !DecodeTexture2D(fetch, data, rgba, width, height)) {
    texture.failed = true;
    ++stats_.texture_failures;
    if (unsupported_formats_.insert(uint32_t(fetch.format)).second) {
      REXLOG_WARN("Renut: Plume does not decode texture format {} yet", uint32_t(fetch.format));
    }
    return 0;
  }
  if (!texture.descriptor) {
    if (next_descriptor_ >= kMaxTextures2D) return 0;
    texture.descriptor = next_descriptor_++;
  }
  if (!texture.texture || texture.content != content) {
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R8G8B8A8_UNORM));
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
  }
  texture.content = content;
  const uint64_t staging = Allocate(uint64_t(width) * height * 4, 512);
  if (staging == UINT64_MAX) return 0;
  std::memcpy(Pointer(staging), rgba.data(), size_t(width) * height * 4);
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get()),
                           RenderTextureCopyLocation::PlacedFootprint(
                               upload_.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1,
                               width, staging));
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_2d_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ);
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

  auto* vs = static_cast<HostShader*>(active_vertex_shader());
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
  uint32_t sampler_descriptor[32] = {};
  for (uint32_t slot = 0; slot < 32; ++slot) {
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
    if (fetch.type != xenos::FetchConstantType::kTexture) continue;
    bool point = fetch.min_filter == xenos::TextureFilter::kPoint;
    uint32_t mode = fetch.clamp_x == xenos::ClampMode::kRepeat           ? 0
                    : fetch.clamp_x == xenos::ClampMode::kMirroredRepeat ? 2
                                                                         : 1;
    sampler_descriptor[slot] = (point ? kSamplerPointWrap : kSamplerLinearWrap) + mode;
    auto found = resolved_.find(fetch.base_address << 12);
    if (found != resolved_.end() && found->second.descriptor) {
      Resolved& r = found->second;
      Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
      texture_descriptor[slot] = r.descriptor;
    } else if (fetch.dimension == xenos::DataDimension::k2DOrStacked) {
      texture_descriptor[slot] = GetGuestTexture(fetch);
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
  std::memcpy(sh + RENUT_SHARED_SAMPLER_OFFSET, sampler_descriptor, sizeof(sampler_descriptor));
  std::memcpy(sh + RENUT_SHARED_BOOLEANS_OFFSET, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              RENUT_BOOL_REGISTER_COUNT / 8);
  for (uint32_t i = 0; i < RENUT_LOOP_CONSTANT_COUNT; ++i) {
    uint32_t v = regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
    int32_t loop[4] = {int32_t(v & 0xFF), int32_t((v >> 8) & 0xFF), int32_t(int8_t((v >> 16) & 0xFF)), 0};
    std::memcpy(sh + RENUT_SHARED_LOOP_OFFSET + i * 16, loop, 16);
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const bool alpha_test = ps && color_control.alpha_test_enable &&
                          color_control.alpha_func != xenos::CompareFunction::kAlways;
  float alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 12, &alpha_ref, 4);

  // Vertex inputs: the layout the bound vertex shader's vfetches describe
  // (DecodeLiveElements); the streams they name are the guest's vertex fetch
  // constants 95 - stream.
  std::vector<RenderInputElement> input_elements;
  std::vector<RenderInputSlot> input_slots;
  std::array<uint64_t, 17> slot_offsets{};
  std::array<uint32_t, 17> slot_sizes{};
  std::array<bool, 17> slot_used{};
  uint32_t swapped[6] = {};  // texcoord, normal, binormal, tangent, blendweight, position
  uint64_t streams[RENUT_VERTEX_STREAM_COUNT] = {};
  for (uint32_t stream = 0; stream < RENUT_VERTEX_STREAM_COUNT; ++stream) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - stream);
    if (fetch.type != xenos::FetchConstantType::kVertex) continue;
    uint64_t offset;
    if (UploadGuest(fetch.address << 2, fetch.size << 2, fetch.endian, offset)) {
      streams[stream] = Address(offset);
      slot_offsets[stream] = offset;
      slot_sizes[stream] = fetch.size << 2;
    }
  }
  std::memcpy(sh + RENUT_SHARED_STREAMS_OFFSET, streams, sizeof(streams));
  bool zero_used = false;
  for (auto [location, integer] : vs->compiled->inputs) {
    const LiveElement* element = nullptr;
    for (const LiveElement& e : vs->elements) {
      if (e.location == location) {
        element = &e;
        break;
      }
    }
    RenderFormat format = element ? VertexFormat(element->format, element->is_signed,
                                                 element->normalized, integer)
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
    if (!slot_used[element->slot]) {
      slot_used[element->slot] = true;
      input_slots.emplace_back(element->slot, element->stride);
    }
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - element->slot);
    if (IsSixteenBitFormat(element->format) && fetch.endian == xenos::Endian::k8in32) {
      // A 32-bit swap leaves 16-bit pairs reversed; swapFloats() undoes it.
      int which = -1;
      switch (element->usage) {
        case 5: which = 0; break;   // TEXCOORD
        case 3: which = 1; break;   // NORMAL
        case 7: which = 2; break;   // BINORMAL
        case 6: which = 3; break;   // TANGENT
        case 1: which = 4; break;   // BLENDWEIGHT
        case 0: which = 5; break;   // POSITION
        default: break;
      }
      if (which >= 0) swapped[which] |= 1u << element->usage_index;
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

  // Viewport. With the guest's viewport transform enabled the shader emits
  // clip space and the host viewport reproduces PA_CL_VPORT_*; with it off the
  // shader emits window coordinates and g_ViewportScaleOffset maps them.
  // XenosRecomp compiles with -fvk-invert-y, so the shaders already negate Y:
  // window y = yo + ys * ndc becomes a viewport of height -2 * ys at yo + ys.
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const Target* extent = colors[0] ? colors[0] : depth;
  RenderViewport viewport;
  float scale_offset[4] = {1.f, 1.f, 0.f, 0.f};
  if (vte.vport_x_scale_ena) {
    float xs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
    float xo = vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.f;
    float ys = vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.f;
    float yo = vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.f;
    float zs = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.f;
    float zo = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.f;
    viewport = RenderViewport(xo - xs, yo + ys, 2.f * xs, -2.f * ys,
                              std::clamp(zo, 0.f, 1.f), std::clamp(zo + zs, 0.f, 1.f));
  } else {
    viewport = RenderViewport(0.f, 0.f, float(extent->width), float(extent->height));
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
  if (scissor_rect.right <= scissor_rect.left || scissor_rect.bottom <= scissor_rect.top) return true;

  // Pipeline.
  const bool depth_enabled = depth && depth_control.z_enable;
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
  const uint32_t spec = alpha_test ? SPEC_CONSTANT_ALPHA_TEST : 0;
  uint64_t key = 0;
  HashCombine(key, vs->compiled);
  HashCombine(key, ps ? ps->compiled : nullptr);
  HashCombine(key, topology);
  HashCombine(key, rect_list);
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
    desc.cullMode = RenderCullMode::NONE;
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
  Target* source = FindTarget(original, edram.format, copying_depth);

  // Source rectangle in pixels: whole tiles from the render target's base,
  // plus the offset within the first tile.
  const uint32_t tile_width = edram.format_is_64bpp ? 40 : 80;
  const uint32_t tiles = edram.base_tiles - original;
  const uint32_t pitch_tiles = std::max(1u, uint32_t(edram.pitch_tiles));
  const uint32_t x = (tiles % pitch_tiles) * tile_width + info.coordinate_info.edram_offset_x_div_8 * 8;
  const uint32_t y = (tiles / pitch_tiles) * 16 + info.coordinate_info.edram_offset_y_div_8 * 8;

  if (source && !copying_depth && x < source->width && y < source->height &&
      info.rb_copy_control.copy_command != xenos::CopyCommand::kNull) {
    const uint32_t w = std::min(width, source->width - x), h = std::min(height, source->height - y);
    const uint32_t dest_w = std::max(uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32u, w);
    const uint32_t dest_h = std::max(uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32u, h);
    const uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
    const uint32_t dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
    Resolved& dest = GetResolved(info.copy_dest_base, source->format, dest_w + dx, dest_h + dy);
    if (dest.texture) {
      Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h), 0, 1);
      list_->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
                               RenderTextureCopyLocation::Subresource(source->texture.get()), dx, dy,
                               0, &box);
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
    Target* depth = FindTarget(info.depth_original_base, info.depth_edram_info.format, true);
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

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  auto* renderer = PlumeRenderer::Get();
  RenderTexture* source = nullptr;
  auto found = resolved_.find(frontbuffer_ptr);
  if (found != resolved_.end() && found->second.texture) {
    Resolved& r = found->second;
    Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
    source = r.texture.get();
    frontbuffer_width = std::min(frontbuffer_width, r.width);
    frontbuffer_height = std::min(frontbuffer_height, r.height);
  }
  list_->end();
  list_open_ = false;
  if (renderer) {
    renderer->SubmitAndPresent(list_.get(), source, frontbuffer_width, frontbuffer_height);
  }
  if (++swaps_ % 300 == 1) Report();
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
