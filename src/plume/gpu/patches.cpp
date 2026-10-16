// Tessellation patch expansion.
#include "internal.h"

namespace renut::gpu {

#include "plume/shaders/depth_stencil_pack.comp.inc"
#include "plume/shaders/patch_expand.comp.inc"
#include "plume/shaders/patch_levels.comp.inc"

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

}  // namespace renut::gpu
