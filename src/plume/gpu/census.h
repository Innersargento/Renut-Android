#pragma once
// GPU backend subsystem Census. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

// The state of a draw that can decide whether it leaves a sample: the shaders,
// depth/stencil, culling, alpha test, clip planes, write masks, viewport.
struct CensusSignature {
  uint64_t vs = 0, ps = 0;
  uint32_t depth_control = 0, stencil = 0, stencil_bf = 0, raster = 0, color_control = 0, alpha_ref = 0;
  uint32_t clip = 0, color_mask = 0, viewport_w = 0, viewport_h = 0, scissor_w = 0, scissor_h = 0;
  uint32_t prim = 0, kind = 0, count_log2 = 0, flags = 0;
};

// renut_plume_zero_census: every draw runs inside an occlusion query and the
// draws are summed by signature, so the ones that never leave a sample, and the
// state they share, show in <files>/plume-zero-census.txt.
struct Census {
  explicit Census(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  enum Flag : uint32_t {
    kColorPass = 1, kAlphaTest = 2, kAlphaToMask = 4, kStencil = 8, kDepth = 16, kDepthWrite = 32,
    kBlend = 64, kBias = 128, kMemExport = 256, kDeferred = 512,
  };

  static constexpr uint32_t kQueries = 16384;
  struct Entry {
    CensusSignature sig;
    uint64_t draws = 0, zero = 0, samples = 0;
  };
  VkQueryPool pool_ = VK_NULL_HANDLE;
  bool active_ = false;
  std::array<std::vector<uint64_t>, kPlumeFramesInFlight> keys_;
  // Where each query's draw sat in its pass (draws recorded in the pass before
  // it), written by the recorder.
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> positions_;
  // Draws and draws with no sample of the GEQUAL colour passes, by position bucket.
  static constexpr uint32_t kBuckets = 8;
  uint64_t position_draws_[kBuckets] = {}, position_zero_[kBuckets] = {};
  std::unordered_map<uint64_t, Entry> entries_;
  uint64_t frames_ = 0, lost_ = 0;

  void Init();
  bool EnsurePool();
  void Shutdown();
  // A query for this draw, or UINT32_MAX when the slot's range is full.
  uint32_t Assign(const CensusSignature& sig);
  // Sums what the slot's last frame answered (it has finished) and resets its range.
  void BeginSlot(uint32_t slot);
  void ResetSlot(uint32_t slot);
  void WriteReport();
};

}  // namespace renut::gpu
