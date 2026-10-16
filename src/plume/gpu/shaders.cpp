// Register writes, shader lookup and pipeline statistics.
#include "internal.h"

namespace renut::gpu {

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

}  // namespace renut::gpu
