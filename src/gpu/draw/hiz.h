#pragma once
// GPU backend subsystem HiZ. See backend.h.
#include "gpu/core/types.h"

namespace renut::gpu {

class GpuBackend;

struct HiZ {
  explicit HiZ(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  std::unordered_map<uint64_t, HizModelEntry> hiz_models_;
  std::array<HizCold, 4096> hiz_cold_{};
  // key -> slot of hiz_pool_. The boxes live in the pool (a deque: addresses stay put), a
  // slot freed by an erase goes on hiz_free_ with its generation moved on.
  std::unordered_map<uint64_t, uint32_t> hiz_boxes_;
  std::deque<HizBox> hiz_pool_;
  std::vector<uint32_t> hiz_free_;
  void HizRelease(uint32_t slot) {
    HizBox& box = hiz_pool_[slot];
    box.valid = false;
    ++box.gen;
    hiz_free_.push_back(slot);
  }
  // Takes a free slot (or a new one) for `box`, which keeps the slot's generation.
  uint32_t HizTake(HizBox& box) {
    uint32_t slot;
    if (!hiz_free_.empty()) {
      slot = hiz_free_.back();
      hiz_free_.pop_back();
    } else {
      slot = uint32_t(hiz_pool_.size());
      hiz_pool_.emplace_back();
    }
    box.gen = hiz_pool_[slot].gen;
    hiz_pool_[slot] = box;
    return slot;
  }
  // 64 KB blocks of guest memory -> keys of the boxes made from them, so a write
  // finds its boxes without walking all of them (entries of boxes already gone
  // are left and ignored).
  std::unordered_multimap<uint32_t, uint64_t> hiz_box_blocks_;
  void HizIndexBox(uint64_t key, const HizBox& box);
  void HizDropBoxes(uint32_t start, uint32_t length);
  HizBox hiz_scratch_box_;
  std::array<HizFront, 8192> hiz_front_{};
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
  // renut_plume_hiz: 0 off, 1 counts what could be dropped, 2 drops the draws whose box lies
  // wholly outside the frustum or the scissor (exact).
  uint32_t mode_ = 0;
};

}  // namespace renut::gpu
