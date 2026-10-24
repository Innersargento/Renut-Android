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
};

}  // namespace renut::gpu
