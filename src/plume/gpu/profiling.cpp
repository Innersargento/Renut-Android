// GPU timestamps and the pair profiler.
#include "internal.h"

namespace renut::gpu {

void PlumeCommandProcessor::WriteTimestamp() {
  if (!OnRecorder()) SyncRecorder();
  // plume-skip bit 67108864 turns the per-pass timing off. It writes ~138
  // timestamps a frame, mostly at render pass boundaries, and a timestamp there
  // can make a tiler flush - so its own cost has to be measurable.
  if (timestamp_pool_ == VK_NULL_HANDLE || (skip_mask_ & 67108864) ||
      timestamps_used_ >= kMaxTimestamps) {
    return;
  }
  vkCmdWriteTimestamp(static_cast<VulkanCommandList*>(L())->vk,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool_,
                      slot_ * kMaxTimestamps + timestamps_used_);
  ++timestamps_used_;
}

// Folds the timings a finished frame left behind into the running totals. The
// interval between two marks is the pass that ran between them.
void PlumeCommandProcessor::ReadTimestamps(uint32_t slot) {
  const uint32_t count = timestamp_count_[slot];
  timestamp_count_[slot] = 0;
  if (timestamp_pool_ == VK_NULL_HANDLE || count < 2) return;
  std::array<uint64_t, kMaxTimestamps> ticks{};
  if (vkGetQueryPoolResults(static_cast<VulkanDevice*>(device_)->vk, timestamp_pool_,
                            slot * kMaxTimestamps, count, count * sizeof(uint64_t), ticks.data(),
                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  const auto& marks = pass_marks_[slot];
  for (uint32_t i = 0; i + 1 < count; ++i) {
    if (ticks[i + 1] <= ticks[i]) continue;
    const uint64_t ns = uint64_t(double(ticks[i + 1] - ticks[i]) * timestamp_ns_);
    gpu_ns_ += ns;
    // Mark i opened the pass that the interval measures; the first interval is
    // whatever ran before any framebuffer was bound.
    uint64_t key = 0;
    uint32_t draws = 0;
    if (i < marks.size()) {
      const PassMark& m = marks[i];
      key = (uint64_t(m.width) << 32) | (uint64_t(m.height) << 8) | (uint64_t(m.colors) << 1) |
            uint64_t(m.depth);
      key |= uint64_t(m.resolve) << 62;
      draws = m.draws;
    }
    PassTotal& total = pass_totals_[key];
    total.ns += ns;
    total.passes += 1;
    total.draws += draws;
  }
}

// A profiled draw's entry and its first timestamp. False when the frame has no
// room for another pair, in which case the draw simply goes unmeasured.
bool PlumeCommandProcessor::PairBegin(const DrawPacket& pk) {
  if (pair_used_ + 2 > kPairQueries) return false;
  PairMark mark;
  mark.vs = pk.vs_hash;
  mark.ps = pk.ps_hash;
  mark.count = pk.count;
  mark.colors = uint8_t(pk.pass_colors);
  mark.depth = pk.pass_depth ? 1 : 0;
  mark.kind = uint8_t(pk.kind);
  mark.switched = bound_pipeline_ != pk.pipeline ? 1 : 0;
  pair_marks_[slot_].push_back(mark);
  PairStamp();
  return true;
}

void PlumeCommandProcessor::PairStamp() {
  vkCmdWriteTimestamp(static_cast<VulkanCommandList*>(list_)->vk,
                      pairprof_mode_ == 2 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      pair_pool_, slot_ * kPairQueries + pair_used_);
  ++pair_used_;
}

// Folds a captured frame into the per-pair sums, and writes the report once the
// last frame of the capture has been read.
void PlumeCommandProcessor::ReadPairTimestamps(uint32_t slot) {
  if (!pair_pending_[slot]) return;
  pair_pending_[slot] = false;
  const uint32_t count = pair_count_[slot];
  pair_count_[slot] = 0;
  const auto& marks = pair_marks_[slot];
  if (pair_pool_ != VK_NULL_HANDLE && count >= 2 && marks.size() * 2 == count) {
    std::vector<uint64_t> ticks(count);
    if (vkGetQueryPoolResults(static_cast<VulkanDevice*>(device_)->vk, pair_pool_, slot * kPairQueries, count,
                              count * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      auto ns = [&](uint64_t from, uint64_t to) {
        return to > from ? uint64_t(double(to - from) * timestamp_ns_) : uint64_t(0);
      };
      for (size_t i = 0; i < marks.size(); ++i) {
        const PairMark& m = marks[i];
        const uint64_t span = ns(ticks[2 * i], ticks[2 * i + 1]);
        PairTotal& total = pair_totals_[{m.vs, m.ps}];
        total.ns += span;
        ++total.draws;
        total.vertices += m.count;
        total.switches += m.switched;
        total.colors = m.colors;
        total.depth = m.depth;
        pair_span_ns_ += span;
        if (i + 1 < marks.size()) pair_gap_ns_ += ns(ticks[2 * i + 1], ticks[2 * i + 2]);
      }
      pair_frame_ns_ += ns(ticks[0], ticks[count - 1]);
      ++pair_frames_;
    }
  }
  if (pairprof_frames_left_ == 0 && std::none_of(pair_pending_.begin(), pair_pending_.end(), [](bool p) { return p; })) {
    WritePairReport();
  }
}

void PlumeCommandProcessor::WritePairReport() {
  const uint32_t frames = pair_frames_;
  const uint32_t mode = pairprof_mode_;
  const uint64_t span_ns = pair_span_ns_, gap_ns = pair_gap_ns_, frame_ns = pair_frame_ns_;
  std::map<std::pair<uint64_t, uint64_t>, PairTotal> totals;
  totals.swap(pair_totals_);
  pair_frames_ = 0;
  pair_span_ns_ = pair_gap_ns_ = pair_frame_ns_ = 0;
  if (!frames || miss_dump_directory_.empty()) return;
  const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));

  struct Row {
    uint64_t vs, ps;
    PairTotal total;
  };
  std::vector<Row> rows;
  PairTotal depth_only, colour;
  for (const auto& [key, total] : totals) {
    rows.push_back({key.first, key.second, total});
    PairTotal& into = total.colors ? colour : depth_only;
    into.ns += total.ns;
    into.draws += total.draws;
    into.switches += total.switches;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.total.ns > b.total.ns; });
  const double per = 1.0 / double(frames);
  const double all = span_ns ? double(span_ns) : 1.0;
  auto us = [&](uint64_t ns) { return double(ns) / 1000.0 * per; };
  auto pct = [&](uint64_t ns) { return 100.0 * double(ns) / all; };

  std::string out;
  out += fmt::format("# plume-pairprof mode {} ({}), {} frames, {} pairs, timestamp period {:.2f} ns\n", mode,
                     mode == 2 ? "bottom of pipe: each draw alone" : "top of pipe: command processor clock",
                     frames, rows.size(), timestamp_ns_);
  uint64_t draws = depth_only.draws + colour.draws;
  out += fmt::format("# per frame: {:.0f} draws, spans {:.0f} us, between draws {:.0f} us, first stamp to last {:.0f} us\n",
                     double(draws) * per, us(span_ns), us(gap_ns), us(frame_ns));
  out += fmt::format("# depth-only: {:.0f} draws {:.0f} us ({:.1f}%, {:.0f}% switch pipeline) | colour: {:.0f} draws {:.0f} us ({:.1f}%, {:.0f}% switch)\n",
                     double(depth_only.draws) * per, us(depth_only.ns), pct(depth_only.ns),
                     depth_only.draws ? 100.0 * double(depth_only.switches) / double(depth_only.draws) : 0.0,
                     double(colour.draws) * per, us(colour.ns), pct(colour.ns),
                     colour.draws ? 100.0 * double(colour.switches) / double(colour.draws) : 0.0);
  out += "# rank    pct   us/frame  draws/frame  avg_us  avg_verts  switch%  c d  vs               ps\n";
  const size_t shown = std::min<size_t>(rows.size(), 150);
  for (size_t i = 0; i < shown; ++i) {
    const Row& r = rows[i];
    const PairTotal& t = r.total;
    out += fmt::format("{:5d} {:6.2f} {:10.1f} {:11.2f} {:7.2f} {:10.0f} {:7.0f}  {} {}  {:016X} {:016X}\n", i + 1,
                       pct(t.ns), us(t.ns), double(t.draws) * per, double(t.ns) / 1000.0 / double(t.draws),
                       double(t.vertices) / double(t.draws), 100.0 * double(t.switches) / double(t.draws),
                       t.colors, t.depth, r.vs, r.ps);
  }
  for (int side = 0; side < 2; ++side) {
    std::map<uint64_t, PairTotal> by;
    for (const Row& r : rows) {
      PairTotal& into = by[side == 0 ? r.vs : r.ps];
      into.ns += r.total.ns;
      into.draws += r.total.draws;
      into.vertices += r.total.vertices;
      into.switches += r.total.switches;
    }
    std::vector<std::pair<uint64_t, PairTotal>> ranked(by.begin(), by.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    out += fmt::format("# by {} shader ({} of them)\n", side == 0 ? "vertex" : "pixel", ranked.size());
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 100); ++i) {
      const PairTotal& t = ranked[i].second;
      out += fmt::format("{:5d} {:6.2f} {:10.1f} {:11.2f} {:7.2f}  {:016X}\n", i + 1, pct(t.ns), us(t.ns),
                         double(t.draws) * per, double(t.ns) / 1000.0 / double(t.draws), ranked[i].first);
    }
  }
  if (FILE* file = std::fopen((root + "/plume-pairprof.txt").c_str(), "w")) {
    std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
  }
  REXLOG_INFO("Renut: Plume pair profile written: {} frames, {} pairs, {:.0f} draws a frame", frames, rows.size(),
              double(draws) * per);
}

}  // namespace renut::gpu
