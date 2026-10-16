#pragma once
// GPU backend subsystem Reporting. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Reporting {
  explicit Reporting(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  void Report();
  // Host memory the cache holds, by category. Printed when the driver's block
  // total moved by 32 MB since the last line (and never twice within 15 frames).
  void ReportMemory();
  uint64_t reported_memory_blocks_ = 0;
  uint64_t shader_cache_hits_ = 0, shader_cache_drops_ = 0;
  std::unordered_map<uint64_t, uint64_t> missed_draws_;
  std::map<std::tuple<std::string, uint64_t, uint64_t, bool>, uint64_t> drops_;
  uint64_t reported_guest_swaps_ = 0;
  std::unordered_map<uint64_t, PassTotal> pass_totals_;
  uint64_t gpu_ns_ = 0;
  // ---- Hi-Z (hiz_model.h) ----
  // files/plume-hiz: 0 off, 1 counts what could be dropped, 2 drops the draws
  // whose box lies wholly outside the frustum or the scissor (exact).
  uint32_t hiz_mode_ = 0;
  std::unordered_map<uint64_t, uint64_t> hiz_clip_states_;
  std::unordered_map<uint64_t, uint64_t> hiz_nomodel_draws_;
  std::unordered_map<uint64_t, uint32_t> hiz_nomodel_why_;
  // Verification runs: the shader pairs of the held draws that put out no
  // samples although the test could not call them hidden, with their counts.
  std::unordered_map<uint64_t, std::array<uint64_t, 3>> hiz_zero_pairs_;
  RecStats rec_stats_;
  uint64_t reported_silent_frames_ = 0;
};

}  // namespace renut::gpu
