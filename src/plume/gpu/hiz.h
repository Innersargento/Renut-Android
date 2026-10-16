#pragma once
// GPU backend subsystem HiZ. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct HiZ {
  explicit HiZ(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  std::unordered_map<uint64_t, HizModelEntry> hiz_models_;
  std::array<HizCold, 4096> hiz_cold_{};
  std::unordered_map<uint64_t, HizBox> hiz_boxes_;
  // 64 KB blocks of guest memory -> keys of the boxes made from them, so a write
  // finds its boxes without walking all of them (entries of boxes already gone
  // are left and ignored).
  std::unordered_multimap<uint32_t, uint64_t> hiz_box_blocks_;
  void HizIndexBox(uint64_t key, const HizBox& box);
  void HizDropBoxes(uint32_t start, uint32_t length);
  HizBox hiz_scratch_box_;
  std::array<HizFront, 8192> hiz_front_{};
  uint32_t hiz_box_generation_ = 1;
  HizModelEntry* HizModelFor(HostShader& vs);
  const HizBox* HizBoxFor(const renut::hiz::Model& model, const xenos::xe_gpu_vertex_fetch_t& fetch,
                          const IndexBufferInfo* index_info, uint32_t index_count, int32_t base_vertex);
  bool HizValidate(HostShader& vs, renut::hiz::Model& model, const xenos::xe_gpu_vertex_fetch_t& fetch,
                   uint32_t vertex_a, uint32_t vertex_b);
  // True when nothing of the draw can reach the target.
  bool HizCullDraw(HostShader& vs, xenos::PrimitiveType prim_type, uint32_t index_count,
                   const IndexBufferInfo* index_info, bool color_pass);
  uint64_t hiz_last_box_key_ = 0;
  bool hiz_stage_timing_ = false;
  // ---- Hi-Z occlusion (see HizFlushWindow) ----
  bool hiz_cond_ = false;
  VkPipeline hiz_pipelines_[3] = {};
  VkShaderModule hiz_modules_[3] = {};
  VkPipelineLayout hiz_layout_ = VK_NULL_HANDLE;
  bool hiz_pipelines_failed_ = false;
  std::unique_ptr<RenderBuffer> hiz_pyramid_;
  std::array<std::unique_ptr<RenderBuffer>, kPlumeFramesInFlight> hiz_pred_, hiz_read_;
  std::array<uint32_t, kPlumeFramesInFlight> hiz_pred_used_{};
  bool hiz_debug_ = false;
  // Debug, from the environment: RENUT_HIZ_FORCE holds every testable draw back,
  // RENUT_HIZ_VERIFY runs the held draws regardless inside occlusion queries.
  bool hiz_force_ = false, hiz_verify_ = false;
  VkQueryPool hiz_query_pool_ = VK_NULL_HANDLE;
  std::array<uint32_t, kPlumeFramesInFlight> hiz_queries_used_{};
  // Tests paid for this frame (windows with held draws, and learning ones), the
  // first kind among them; the learning frames and how often they come.
  uint32_t hiz_tests_frame_ = 0, hiz_tests_remembered_ = 0;
  uint32_t hiz_learn_left_ = 0, hiz_learn_period_ = 8;
  uint64_t hiz_gain_answered_ = 0, hiz_gain_hidden_ = 0, hiz_total_answered_ = 0, hiz_total_hidden_ = 0;
  std::array<uint32_t, kPlumeFramesInFlight> hiz_debug_pending_{};
  std::array<std::vector<std::array<float, 5>>, kPlumeFramesInFlight> hiz_debug_rects_;
  std::vector<HizRecord> hiz_records_;
  std::vector<uint32_t> hiz_held_;
  std::array<std::vector<HizPending>, kPlumeFramesInFlight> hiz_pending_;
  std::unordered_map<uint64_t, HizHistory> hiz_history_;
  bool HizCreatePipelines();
  bool HizEnsureResources();
  bool HizRunTest(Target& target, uint32_t count, uint32_t* base_out);
  bool HizFlushWindow(size_t n);
  void HizReadResults(uint32_t slot);
  void HizDestroy();
};

}  // namespace renut::gpu
