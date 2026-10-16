#pragma once
// GPU backend subsystem Profiling. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Profiling {
  explicit Profiling(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  double timestamp_ns_ = 0.0;
  std::array<uint32_t, kPlumeFramesInFlight> timestamp_count_{};
  uint32_t timestamps_used_ = 0;
  void WriteTimestamp();
  void ReadTimestamps(uint32_t slot);
  std::array<std::vector<PairMark>, kPlumeFramesInFlight> pair_marks_;
  std::array<uint32_t, kPlumeFramesInFlight> pair_count_{};
  std::array<bool, kPlumeFramesInFlight> pair_pending_{};
  std::map<std::pair<uint64_t, uint64_t>, PairTotal> pair_totals_;
  uint32_t pair_used_ = 0, pair_frames_ = 0;
  uint64_t pair_span_ns_ = 0, pair_gap_ns_ = 0, pair_frame_ns_ = 0;
  void PairStamp();
  void ReadPairTimestamps(uint32_t slot);
  void WritePairReport();
  bool PairBegin(const DrawPacket& pk);
};

}  // namespace renut::gpu
