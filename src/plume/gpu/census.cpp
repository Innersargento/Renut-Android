// Census of the draws that leave no sample (renut_plume_zero_census).
#include "internal.h"

namespace renut::gpu {

void Census::Init() {}

// The query pool, made the first time the census is turned on.
bool Census::EnsurePool() {
  if (pool_ != VK_NULL_HANDLE) return true;
  VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_OCCLUSION;
  info.queryCount = kQueries * kPlumeFramesInFlight;
  for (auto& positions : positions_) positions.assign(kQueries, 0);
  if (vkCreateQueryPool(static_cast<VulkanDevice*>(backend.device_)->vk, &info, nullptr, &pool_) != VK_SUCCESS) {
    pool_ = VK_NULL_HANDLE;
    REXLOG_WARN("Renut: Plume zero-sample census: no query pool");
    return false;
  }
  return true;
}

void Census::Shutdown() {
  if (pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(static_cast<VulkanDevice*>(backend.device_)->vk, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
}

uint32_t Census::Assign(const CensusSignature& sig) {
  auto& keys = keys_[backend.slot_];
  if (keys.size() >= kQueries) {
    ++lost_;
    return UINT32_MAX;
  }
  uint64_t key = 14695981039346656037ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&sig);
  for (size_t i = 0; i < sizeof(sig); ++i) key = (key ^ bytes[i]) * 1099511628211ull;
  auto [it, fresh] = entries_.try_emplace(key);
  if (fresh) it->second.sig = sig;
  keys.push_back(key);
  return backend.slot_ * kQueries + uint32_t(keys.size() - 1);
}

void Census::BeginSlot(uint32_t slot) {
  // The cvar is followed live: on starts a fresh count, off writes the last report.
  const bool want = REXCVAR_GET(renut_plume_zero_census);
  if (want != active_) {
    if (want && EnsurePool()) {
      entries_.clear();
      frames_ = lost_ = 0;
      std::fill(std::begin(position_draws_), std::end(position_draws_), 0);
      std::fill(std::begin(position_zero_), std::end(position_zero_), 0);
      active_ = true;
      REXLOG_INFO("Renut: Plume zero-sample census on ({} queries a frame)", kQueries);
    } else if (!want) {
      if (frames_) WriteReport();
      active_ = false;
      REXLOG_INFO("Renut: Plume zero-sample census off after {} frames", frames_);
    }
  }
  auto& keys = keys_[slot];
  if (pool_ == VK_NULL_HANDLE || keys.empty()) return;
  std::vector<uint64_t> samples(keys.size(), 0);
  if (vkGetQueryPoolResults(static_cast<VulkanDevice*>(backend.device_)->vk, pool_, slot * kQueries,
                            uint32_t(keys.size()), samples.size() * sizeof(uint64_t), samples.data(),
                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
    for (size_t i = 0; i < keys.size(); ++i) {
      Entry& e = entries_[keys[i]];
      ++e.draws;
      e.samples += samples[i];
      if (!samples[i]) ++e.zero;
      reg::RB_DEPTHCONTROL dc;
      dc.value = e.sig.depth_control;
      if ((e.sig.flags & kColorPass) && (e.sig.flags & kDepth) && e.sig.color_mask &&
          dc.zfunc == xenos::CompareFunction::kGreaterEqual) {
        const uint32_t pos = positions_[slot][i];
        uint32_t b = 0;
        static constexpr uint32_t kEdges[kBuckets - 1] = {1, 10, 50, 100, 200, 400, 800};
        while (b < kBuckets - 1 && pos >= kEdges[b]) ++b;
        ++position_draws_[b];
        if (!samples[i]) ++position_zero_[b];
      }
    }
    ++frames_;
  }
  keys.clear();
  if (frames_ && frames_ % 300 == 0) WriteReport();
}

void Census::ResetSlot(uint32_t slot) {
  if (!active_) return;
  vkCmdResetQueryPool(static_cast<VulkanCommandList*>(backend.recorder.L())->vk, pool_, slot * kQueries, kQueries);
}

namespace {

const char* Func(uint32_t f) {
  static const char* names[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
  return names[f & 7];
}

const char* Op(uint32_t o) {
  static const char* names[] = {"keep", "zero", "replace", "incsat", "decsat", "invert", "incwrap", "decwrap"};
  return names[o & 7];
}

std::string Describe(const CensusSignature& s) {
  reg::RB_DEPTHCONTROL dc;
  dc.value = s.depth_control;
  reg::RB_STENCILREFMASK sr;
  sr.value = s.stencil;
  reg::PA_SU_SC_MODE_CNTL ra;
  ra.value = s.raster;
  reg::RB_COLORCONTROL cc;
  cc.value = s.color_control;
  reg::PA_CL_CLIP_CNTL cl;
  cl.value = s.clip;
  float alpha_ref;
  std::memcpy(&alpha_ref, &s.alpha_ref, 4);
  std::string out = fmt::format("vs={:016X} ps={:016X} {} prim={} kind={} count~2^{} vp={}x{} sc={}x{} mask={:X}",
                                s.vs, s.ps, (s.flags & Census::kColorPass) ? "colour" : "depth-only", s.prim, s.kind,
                                s.count_log2, s.viewport_w, s.viewport_h, s.scissor_w, s.scissor_h, s.color_mask);
  if (s.flags & Census::kDepth) {
    out += fmt::format(" z={}{}", Func(uint32_t(dc.zfunc)), (s.flags & Census::kDepthWrite) ? "+w" : "");
  }
  if (s.flags & Census::kStencil) {
    out += fmt::format(" stencil={} ref={:02X} mask={:02X} wmask={:02X} fail={} zpass={} zfail={}",
                       Func(uint32_t(dc.stencilfunc)), uint32_t(sr.stencilref), uint32_t(sr.stencilmask),
                       uint32_t(sr.stencilwritemask), Op(uint32_t(dc.stencilfail)), Op(uint32_t(dc.stencilzpass)),
                       Op(uint32_t(dc.stencilzfail)));
    if (dc.backface_enable) {
      reg::RB_STENCILREFMASK bf;
      bf.value = s.stencil_bf;
      out += fmt::format(" bf={} ref={:02X} mask={:02X}", Func(uint32_t(dc.stencilfunc_bf)), uint32_t(bf.stencilref),
                         uint32_t(bf.stencilmask));
    }
  }
  if (ra.cull_front || ra.cull_back) {
    out += fmt::format(" cull={}{} face={}", ra.cull_front ? "f" : "", ra.cull_back ? "b" : "", uint32_t(ra.face));
  }
  if (s.flags & (Census::kAlphaTest | Census::kAlphaToMask)) {
    out += fmt::format(" alpha={} ref={}{}", Func(uint32_t(cc.alpha_func)), alpha_ref,
                       (s.flags & Census::kAlphaToMask) ? " a2m" : "");
  }
  const uint32_t ucp = s.clip & 0x3F;
  if (ucp) out += fmt::format(" ucp={:X}{}", ucp, cl.ucp_cull_only_ena ? " cull-only" : "");
  if (cl.vtx_kill_or) out += " vtx_kill_or";
  if (cl.clip_disable) out += " clip_disable";
  if (s.flags & Census::kBlend) out += " blend";
  if (s.flags & Census::kBias) out += " bias";
  if (s.flags & Census::kMemExport) out += " memexport";
  if (s.flags & Census::kDeferred) out += " deferred";
  return out;
}

}  // namespace

void Census::WriteReport() {
  if (backend.miss_dump_directory_.empty()) return;
  const std::string path =
      backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/')) + "/plume-zero-census.txt";
  FILE* out = std::fopen(path.c_str(), "w");
  if (!out) return;
  uint64_t draws[2] = {}, zero[2] = {};
  std::vector<const Entry*> sorted;
  for (const auto& [key, e] : entries_) {
    const int colour = (e.sig.flags & kColorPass) ? 1 : 0;
    draws[colour] += e.draws;
    zero[colour] += e.zero;
    if (e.zero) sorted.push_back(&e);
  }
  std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) { return a->zero > b->zero; });
  std::fprintf(out, "frames=%llu signatures=%zu lost=%llu\n", (unsigned long long)frames_, entries_.size(),
               (unsigned long long)lost_);
  for (int c = 1; c >= 0; --c) {
    std::fprintf(out, "%s: %llu draws, %llu with no sample (%.1f%%), %.1f a frame\n", c ? "colour" : "depth-only",
                 (unsigned long long)draws[c], (unsigned long long)zero[c], draws[c] ? 100.0 * zero[c] / draws[c] : 0.0,
                 frames_ ? double(zero[c]) / frames_ : 0.0);
  }
  static const char* kBucketNames[kBuckets] = {"0", "1-9", "10-49", "50-99", "100-199", "200-399", "400-799", "800+"};
  std::fprintf(out, "\nGEQUAL colour draws by position in their pass (draws before them):\n");
  for (uint32_t b = 0; b < kBuckets; ++b) {
    std::fprintf(out, "  %-8s %10llu draws, %10llu with no sample (%.1f%%)\n", kBucketNames[b],
                 (unsigned long long)position_draws_[b], (unsigned long long)position_zero_[b],
                 position_draws_[b] ? 100.0 * position_zero_[b] / position_draws_[b] : 0.0);
  }
  std::fprintf(out, "\nzero  draws  zero%%  per-frame  signature\n");
  for (size_t i = 0; i < sorted.size(); ++i) {
    const Entry& e = *sorted[i];
    std::fprintf(out, "%llu %llu %.1f %.1f %s\n", (unsigned long long)e.zero, (unsigned long long)e.draws,
                 100.0 * e.zero / e.draws, frames_ ? double(e.zero) / frames_ : 0.0, Describe(e.sig).c_str());
  }
  std::fclose(out);
}

}  // namespace renut::gpu
