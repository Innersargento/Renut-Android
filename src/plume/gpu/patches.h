#pragma once
// GPU backend subsystem Patches. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Patches {
  explicit Patches(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  VkPipeline pack_pipeline_ = VK_NULL_HANDLE;
  bool CreatePackPipeline();
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
