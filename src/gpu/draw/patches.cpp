// Tessellation patch expansion.
#include "gpu/core/internal.h"
#include "plume/shaders/host_shaders.h"

namespace renut::gpu {

bool Patches::CreatePackPipeline() {
  if (pack_pipeline_ != VK_NULL_HANDLE) return true;
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = sizeof(k_depth_stencil_pack_comp);
  module_info.pCode = k_depth_stencil_pack_comp;
  if (vkCreateShaderModule(vk_device, &module_info, nullptr, &backend.pack_module_) != VK_SUCCESS) return false;
  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &backend.pack_layout_) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = backend.pack_module_;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = backend.pack_layout_;
  if (vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pack_pipeline_) !=
      VK_SUCCESS) {
    pack_pipeline_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool Patches::CreateDepthResolvePipeline() {
  if (dres_pipeline_ != VK_NULL_HANDLE) return true;
  if (dres_failed_) return false;
  dres_failed_ = true;
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = sizeof(k_depth_resolve_comp);
  module_info.pCode = k_depth_resolve_comp;
  if (vkCreateShaderModule(vk_device, &module_info, nullptr, &dres_module_) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding bindings[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 3;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(vk_device, &set_info, nullptr, &dres_set_layout_) != VK_SUCCESS) return false;
  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &dres_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &dres_layout_) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = dres_module_;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = dres_layout_;
  if (vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &dres_pipeline_) != VK_SUCCESS) {
    dres_pipeline_ = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kDresSets},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDresSets}};
  for (uint32_t i = 0; i < kPlumeFramesInFlight; ++i) {
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = kDresSets;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &dres_pool_[i]) != VK_SUCCESS) {
      dres_pool_[i] = VK_NULL_HANDLE;
      vkDestroyPipeline(vk_device, dres_pipeline_, nullptr);
      dres_pipeline_ = VK_NULL_HANDLE;
      return false;
    }
  }
  dres_failed_ = false;
  return true;
}

// The slot's frame is over (its fence was waited for): the views and sets it made go.
void Patches::ReleaseDepthResolveSlot(uint32_t slot) {
  if (dres_views_[slot].empty() && !dres_used_[slot]) return;
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  for (VkImageView view : dres_views_[slot]) vkDestroyImageView(vk_device, view, nullptr);
  dres_views_[slot].clear();
  if (dres_pool_[slot] != VK_NULL_HANDLE) vkResetDescriptorPool(vk_device, dres_pool_[slot], 0);
  dres_used_[slot] = 0;
}

void Patches::DestroyDepthResolve() {
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  for (uint32_t i = 0; i < kPlumeFramesInFlight; ++i) {
    for (VkImageView view : dres_views_[i]) vkDestroyImageView(vk_device, view, nullptr);
    dres_views_[i].clear();
    if (dres_pool_[i] != VK_NULL_HANDLE) vkDestroyDescriptorPool(vk_device, dres_pool_[i], nullptr);
    dres_pool_[i] = VK_NULL_HANDLE;
    dres_used_[i] = 0;
  }
  if (dres_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, dres_pipeline_, nullptr);
  if (dres_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, dres_layout_, nullptr);
  if (dres_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(vk_device, dres_set_layout_, nullptr);
  if (dres_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, dres_module_, nullptr);
  dres_pipeline_ = VK_NULL_HANDLE;
  dres_layout_ = VK_NULL_HANDLE;
  dres_set_layout_ = VK_NULL_HANDLE;
  dres_module_ = VK_NULL_HANDLE;
  dres_failed_ = false;
}

bool Patches::CreatePatchPipelines() {
  if (patch_pipelines_[1] != VK_NULL_HANDLE) return true;
  if (patch_pipelines_failed_) return false;
  patch_pipelines_failed_ = true;
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
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
    if (vkCreateShaderModule(vk_device, &module_info, nullptr, &backend.patch_modules_[i]) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = backend.patch_modules_[i];
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
bool Patches::ExpandAdaptivePatches(uint32_t factors_base, xenos::Endian endian, uint32_t patches,
                                                  uint32_t min_level, uint32_t max_level, AdaptiveDraw& out) {
  if (!CreatePatchPipelines()) return false;
  RenderBuffer* factors = nullptr;
  uint64_t factors_offset = 0;
  if (!backend.streams.BindStream(factors_base, patches * 12, endian, factors, factors_offset) || !factors) return false;
  const uint64_t factors_address = factors->getDeviceAddress() + factors_offset;
  const uint32_t levels = min_level | (max_level << 8);
  backend.draw.Trace("  adaptive patches {} factors {:08X} levels {}..{}", patches, factors_base, min_level, max_level);
  if (patch_reuse_.frame == backend.swaps_ && patch_reuse_.factors == factors_address && patch_reuse_.patches == patches &&
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
  std::unique_ptr<RenderBuffer>& buffer = patch_out_[backend.slot_];
  uint64_t at = (patch_out_used_ + 15) & ~uint64_t(15);
  if (!buffer || at + need > patch_out_capacity_[backend.slot_]) {
    const uint64_t capacity = std::max({need, patch_out_capacity_[backend.slot_] * 2, uint64_t(4) << 20});
    auto made = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        capacity, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::STORAGE |
                      RenderBufferFlag::DEVICE_ADDRESSABLE));  // the layer adds the indirect usage
    if (!made) return false;
    // Draws recorded earlier this frame may still read the old one.
    if (buffer) backend.streams.retired_buffers_[backend.slot_].push_back(std::move(buffer));
    buffer = std::move(made);
    patch_out_capacity_[backend.slot_] = capacity;
    at = 0;
  }
  const uint64_t records_at = at;
  const uint64_t args_at = records_at + records_bytes;
  const uint64_t points_at = (args_at + 32 + 15) & ~uint64_t(15);
  const uint64_t indices_at = points_at + points * 16;
  patch_out_used_ = indices_at + indices * 4;
  const uint64_t base = buffer->getDeviceAddress();

  // Raw Vulkan, outside the render pass; it reopens at the draw.
  auto* vulkan = static_cast<VulkanCommandList*>(backend.recorder.L());
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
  backend.targets.pushed_constants_[0] = backend.targets.pushed_constants_[1] = backend.targets.pushed_constants_[2] = UINT64_MAX;
  ++backend.stats_.adaptive_patch_draws;

  out.buffer = buffer.get();
  out.points_offset = points_at;
  out.points_bytes = points * 16;
  out.args_offset = args_at;
  patch_reuse_.frame = backend.swaps_;
  patch_reuse_.factors = factors_address;
  patch_reuse_.patches = patches;
  patch_reuse_.levels = levels;
  patch_reuse_.exports = export_draws_total_;
  patch_reuse_.draw = out;
  return true;
}

}  // namespace renut::gpu
