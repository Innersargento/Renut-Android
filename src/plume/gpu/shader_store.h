#pragma once
// Recompiled shaders: HostShader and the store that finds them.
#include "base.h"

namespace renut::gpu {


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

class HostShader : public Shader {
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

}  // namespace renut::gpu
