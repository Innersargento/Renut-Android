// Free helpers of the Plume GPU: format and blend translation, shader inputs, pipeline
// creation and the recipe codec. Declared in internal.h.
#include "internal.h"

namespace renut::gpu {

const UcodeMapEntry kUcodeMap[] = {
#include "shader_ucode_map.inc"
};

std::span<const UcodeMapEntry> UcodeMap() { return kUcodeMap; }

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

uint32_t TextureMask(const Shader& shader) {
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

std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(RenderDevice* device, const RenderGraphicsPipelineDesc& desc) {
  auto pipeline = device->createGraphicsPipeline(desc);
  if (pipeline && static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk == VK_NULL_HANDLE) return nullptr;
  return pipeline;
}

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

}  // namespace renut::gpu
