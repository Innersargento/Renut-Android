#pragma once
// GPU backend subsystem Patches. See backend.h.
#include "gpu/core/types.h"

namespace renut::gpu {

class GpuBackend;

struct Patches {
  explicit Patches(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  VkPipeline pack_pipeline_ = VK_NULL_HANDLE;
  bool CreatePackPipeline();
  // A depth resolve with its stencil straight into the RG32F texture (depth_resolve.comp): the pipeline, its
  // descriptor set layout, and per frame slot a pool for the sets it uses and the stencil views made for them
  // (freed when the slot comes round again). Resolve::ResolveDepthStencil runs it.
  bool CreateDepthResolvePipeline();
  void ReleaseDepthResolveSlot(uint32_t slot);
  void DestroyDepthResolve();
  VkShaderModule dres_module_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout dres_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout dres_layout_ = VK_NULL_HANDLE;
  VkPipeline dres_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool dres_pool_[kPlumeFramesInFlight] = {};
  std::vector<VkImageView> dres_views_[kPlumeFramesInFlight];
  uint32_t dres_used_[kPlumeFramesInFlight] = {};
  static constexpr uint32_t kDresSets = 8;
  bool dres_failed_ = false;
  bool CreatePatchPipelines();
  bool ExpandAdaptivePatches(uint32_t factors_base, xenos::Endian endian, uint32_t patches, uint32_t min_level,
                             uint32_t max_level, AdaptiveDraw& out);
  VkPipelineLayout patch_layout_ = VK_NULL_HANDLE;
  VkPipeline patch_pipelines_[2] = {};
  bool patch_pipelines_failed_ = false;
  std::unique_ptr<RenderBuffer> patch_out_[kPlumeFramesInFlight];
  uint64_t patch_out_capacity_[kPlumeFramesInFlight] = {};
  uint64_t patch_out_used_ = 0;
  PatchReuse patch_reuse_;
  uint64_t export_draws_total_ = 0;
};

}  // namespace renut::gpu
