// Memory and statistics reports.
#include "internal.h"

namespace renut::gpu {

void PlumeCommandProcessor::ReportMemory() {
  auto mb = [](uint64_t bytes) { return bytes >> 20; };
  // Base level plus the rest of the chain, in the 4-byte form every guest
  // texture is decoded to.
  auto chain = [](uint64_t base, uint32_t levels, uint32_t num, uint32_t den) {
    return levels > 1 ? base * num / den : base;
  };
  uint64_t tex2d = 0, tex2d_n = 0, tex2d_bc_n = 0, stacks = 0, volumes = 0, cubes = 0, resolved = 0, resolved_n = 0, target_bytes = 0;
  for (const auto& [key, t] : guest_textures_) {
    if (!t.texture) continue;
    tex2d += HostTextureBytes(t);
    ++tex2d_n;
    if (t.bc != BcKind::kNone) ++tex2d_bc_n;
  }
  for (const auto& [key, t] : guest_stacks_) {
    if (t.texture) stacks += chain(uint64_t(t.width) * t.height * t.depth * 4, t.levels, 4, 3);
  }
  for (const auto& [key, t] : guest_volumes_) {
    if (t.texture) volumes += chain(uint64_t(t.width) * t.height * t.depth * 4, t.levels, 8, 7);
  }
  for (const auto& [key, t] : guest_cubes_) {
    if (t.texture) cubes += chain(uint64_t(t.width) * t.height * 6 * 4, t.levels, 4, 3);
  }
  for (const auto& [key, r] : resolved_) {
    if (r.texture) {
      resolved += uint64_t(r.width) * r.height * RenderFormatSize(r.format);
      ++resolved_n;
    }
    if (r.fitted) resolved += uint64_t(r.width) * r.height * RenderFormatSize(r.format);
  }
  for (const auto& [key, t] : targets_) {
    if (t.texture) target_bytes += uint64_t(t.width) * t.height * RenderFormatSize(t.format);
  }
  uint64_t export_bytes = 0;
  for (const auto& [key, e] : exports_) export_bytes += e.size;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(static_cast<VulkanDevice*>(device_)->allocator, budgets);
  uint64_t blocks = 0, allocations = 0;
  for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) {
    blocks += budgets[i].statistics.blockBytes;
    allocations += budgets[i].statistics.allocationBytes;
  }
  const uint64_t previous = reported_memory_blocks_;
  if (blocks + (32ull << 20) > previous && blocks < previous + (32ull << 20)) return;
  reported_memory_blocks_ = blocks;
  uint64_t rss_kb = 0, swap_kb = 0;
  if (FILE* status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, status)) {
      std::sscanf(line, "VmRSS: %llu kB", reinterpret_cast<unsigned long long*>(&rss_kb));
      std::sscanf(line, "VmSwap: %llu kB", reinterpret_cast<unsigned long long*>(&swap_kb));
    }
    std::fclose(status);
  }
  REXLOG_INFO(
      "Renut: Plume memory @{}: vma blocks {} MB allocations {} MB | tex2d {} MB ({}, {} as BC) stacks {} MB "
      "volumes {} MB cubes {} MB resolved {} MB ({}) targets {} MB ({}) kept {} MB exports {} MB "
      "upload {} MB | rss {} MB swap {} MB",
      swaps_, mb(blocks), mb(allocations), mb(tex2d), tex2d_n, tex2d_bc_n, mb(stacks), mb(volumes), mb(cubes),
      mb(resolved), resolved_n, mb(target_bytes), targets_.size(), mb(kept_bytes_), mb(export_bytes),
      mb(upload_slice_size_ * kPlumeFramesInFlight), rss_kb >> 10, swap_kb >> 10);
}

void PlumeCommandProcessor::Report() {
  SyncRecorder();
  stats_.drawn += rec_stats_.drawn;
  stats_.pipeline_binds += rec_stats_.pipeline_binds;
  stats_.hiz_predicated_draws += rec_stats_.predicated;
  stats_.passes += rec_stats_.passes;
  rec_stats_ = {};
  REXLOG_INFO(
      "Renut: Plume frame {}: draws={} drawn={} no_vs={} no_ps={} bad_shader={} skipped_prim={} "
      "no_target={} resolves={} clears={} passes={} barriers={} pipeline_binds={} vertices={} gs_draws={} patch_draws={} patch_vertices={} adaptive={} split(color={} depth={} read={} src={} dst={}) pipelines={} targets={} resolved={} shaders={} "
      "missing_ucode={} upload_overflow={} upload_used={}KB textures={} decoded={} "
      "texture_failures={} frame_ms={} pipeline_ms={} submit_ms={} guest_swaps={} "
      "const_reused={} export(draws={} ranges={}/{} reads={} merges={} refreshes={} barriers={}) "
      "cp_cpu_ms={} slot_wait_ms={} stream_mb={} constant_mb={} kept(hits={} new_mb={} drops={} unwatchable={} total_mb={} streams={}) "
      "bands(skipped={} moved={} widened={})",
      swaps_, stats_.draws, stats_.drawn, stats_.no_vs, stats_.no_ps, stats_.bad_shader,
      stats_.skipped_prim, stats_.no_target, stats_.resolves, stats_.clears, stats_.passes, stats_.barriers,
      stats_.pipeline_binds, stats_.vertices, stats_.gs_draws, stats_.patch_draws, stats_.patch_vertices, stats_.adaptive_patch_draws,
      stats_.split_by[uint32_t(RenderTextureLayout::COLOR_WRITE) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::DEPTH_WRITE) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::SHADER_READ) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::COPY_SOURCE) & 7],
      stats_.split_by[uint32_t(RenderTextureLayout::COPY_DEST) & 7],
      stats_.pipelines,
      targets_.size(), resolved_.size(), shaders_.size(), missed_.size(), stats_.overflow,
      (upload_used_ - slice_base_) >> 10, guest_textures_.size(), stats_.textures_decoded,
      stats_.texture_failures, stats_.frame_us / 1000, stats_.pipeline_us / 1000,
      stats_.submit_us / 1000, renut_guest_swap_count() - reported_guest_swaps_,
      stats_.constants_reused, stats_.export_draws, exports_.size(), stats_.export_ranges,
      stats_.export_reads, stats_.export_merges, stats_.export_refreshes, stats_.export_barriers,
      stats_.cp_cpu_us / 1000, stats_.slot_wait_us / 1000, stats_.stream_bytes >> 20,
      stats_.constant_bytes >> 20, stats_.kept_hits, stats_.kept_bytes_made >> 20, stats_.kept_drops,
      stats_.kept_unwatchable, kept_bytes_ >> 20, kept_streams_.size(), stats_.band_skipped,
      stats_.band_moved, stats_.band_widened);
  REXLOG_INFO("Renut: Plume shader cache ({}): {} hits, {} entries, {} dropped by writes", shader_cache_ ? "on" : "off",
              shader_cache_hits_, shader_watch_.size(), shader_cache_drops_);
  if (stats_.defer_windows) {
    REXLOG_INFO("Renut: Plume windows closed by: elsewhere {} ({} draws), state change {} ({}), full {} ({}), "
                "immediate draw {} ({}), the draw's own needs {} ({})",
                stats_.flush_windows[0], stats_.flush_draws[0], stats_.flush_windows[1], stats_.flush_draws[1],
                stats_.flush_windows[2], stats_.flush_draws[2], stats_.flush_windows[3], stats_.flush_draws[3],
                stats_.flush_windows[4], stats_.flush_draws[4]);
    REXLOG_INFO("Renut: Plume deferred draws (mode {}): {} draws in {} windows, pipeline changes {} -> {}",
                defer_mode_, stats_.deferred_draws, stats_.defer_windows, stats_.defer_binds_before,
                stats_.defer_binds_after);
  }
  if (hiz_mode_ && (stats_.hiz_seen[0] || stats_.hiz_seen[1])) {
    REXLOG_INFO(
        "Renut: Plume Hi-Z (mode {}): colour seen={} boxed={} no_model={} no_box={} frustum={} scissor={} | "
        "depth-only seen={} boxed={} no_model={} no_box={} frustum={} scissor={} | dropped={} | models {}/{} valid "
        "({} rejected, {} ms), boxes {} made ({} ms, {} reused) | skipped: no_indices={} prim={} memexport={} clip_state={} "
        "const_base={}",
        hiz_mode_, stats_.hiz_seen[1], stats_.hiz_eligible[1], stats_.hiz_no_model[1], stats_.hiz_no_box[1],
        stats_.hiz_frustum[1], stats_.hiz_scissor[1], stats_.hiz_seen[0], stats_.hiz_eligible[0],
        stats_.hiz_no_model[0], stats_.hiz_no_box[0], stats_.hiz_frustum[0], stats_.hiz_scissor[0],
        stats_.hiz_culled_total, stats_.hiz_models_valid, stats_.hiz_models, stats_.hiz_model_rejected,
        stats_.hiz_model_us / 1000, stats_.hiz_boxes_made, stats_.hiz_box_us / 1000, stats_.hiz_box_hits,
        stats_.hiz_why[0], stats_.hiz_why[1], stats_.hiz_why[2], stats_.hiz_why[3], stats_.hiz_why[4]);
  }
  if (hiz_mode_ && !hiz_nomodel_draws_.empty()) {
    std::vector<std::pair<uint64_t, uint64_t>> ranked;
    for (const auto& [key, count] : hiz_nomodel_draws_) ranked.emplace_back(count, key);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string line = "Renut: Plume Hi-Z shaders without a model (vs hash/why/draws):";
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 12); ++i) {
      uint64_t hash = ranked[i].second;
      uint32_t why = 0;
      for (uint32_t w = 0; w < 16; ++w) {
        auto it = hiz_nomodel_why_.find(hash ^ w);
        if (it != hiz_nomodel_why_.end() && it->second == w) { why = w; hash ^= w; break; }
      }
      line += fmt::format(" [{:016X}/{}/{}]", hash, why, ranked[i].first);
    }
    REXLOG_INFO("{}", line);
    hiz_nomodel_draws_.clear();
  }
  if (hiz_mode_ && !hiz_clip_states_.empty()) {
    std::vector<std::pair<uint64_t, uint64_t>> ranked;
    for (const auto& [state, count] : hiz_clip_states_) ranked.emplace_back(count, state);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string line = "Renut: Plume Hi-Z skipped clip states (vte/clip):";
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 6); ++i) {
      line += fmt::format(" [{:08X}/{:08X} x{}]", uint32_t(ranked[i].second >> 32), uint32_t(ranked[i].second),
                          ranked[i].first);
    }
    REXLOG_INFO("{}", line);
    hiz_clip_states_.clear();
  }
  if (hiz_mode_ >= 3) {
    REXLOG_INFO(
        "Renut: Plume Hi-Z occlusion: {} windows tested ({} failed), {} draws tested, {} held back, of which {} "
        "answered hidden and {} visible; {} answered ({} hidden in all); {} draws run under conditional rendering; "
        "history {} keys | boxed draws {} (not deferred {}, biased {}, other depth func {}, no depth {}); windows {} "
        "(not tested {} holding {} draws); learning every {} frames; verified {} held draws: {} violations, {} "
        "visible answers with no samples ({} of them over depth still at the far plane)",
        stats_.hiz_windows_tested, stats_.hiz_test_failed, stats_.hiz_records, stats_.hiz_held,
        stats_.hiz_held_skipped, stats_.hiz_held_visible, stats_.hiz_answered, stats_.hiz_occluded,
        stats_.hiz_predicated_draws, hiz_history_.size(), stats_.hiz_boxed_draws, stats_.hiz_why_not[0],
        stats_.hiz_why_not[1], stats_.hiz_why_not[2], stats_.hiz_why_not[3], stats_.hiz_windows_all,
        stats_.hiz_small_windows, stats_.hiz_small_draws, hiz_learn_period_, stats_.hiz_verified,
        stats_.hiz_violations, stats_.hiz_missed_hidden, stats_.hiz_missed_empty);
  }
  if (!hiz_zero_pairs_.empty()) {
    std::vector<std::array<uint64_t, 3>> ranked;
    uint64_t total = 0;
    for (const auto& [key, entry] : hiz_zero_pairs_) {
      ranked.push_back(entry);
      total += entry[0];
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a[0] > b[0]; });
    std::string line = fmt::format("Renut: Plume Hi-Z draws with no samples that the test could not call hidden ({}):", total);
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 8); ++i) {
      line += fmt::format(" [vs {:016X} ps {:016X} x{}]", ranked[i][1], ranked[i][2], ranked[i][0]);
    }
    REXLOG_INFO("{}", line);
    hiz_zero_pairs_.clear();
  }
  if (bc_supported_ && bc_mode_) {
    REXLOG_INFO("Renut: Plume BC textures: {} of the {} 2D uploads were blocks, not decoded pixels",
                stats_.textures_bc, stats_.textures_decoded);
  }
  if (shadow_min_indices_) {
    REXLOG_INFO("Renut: Plume shadow casters under {} indices dropped: {}", shadow_min_indices_,
                stats_.shadow_culled);
  }
  if (depth_alias_ || stats_.depth_aliased) {
    REXLOG_INFO("Renut: Plume depth maps in place ({}): {} resolves without a copy, {} copied back, {} dropped by a clear",
                depth_alias_ ? "on" : "off", stats_.depth_aliased, stats_.depth_restored, stats_.depth_dropped);
  }
  // Keep what the driver compiled this interval, so the next run of this scene
  // does not spend its frames inside vkCreateGraphicsPipelines. Android rarely
  // shuts the process down cleanly, so waiting for the device's own save would
  // usually lose the blob.
  if (gpu_ns_) {
    std::vector<std::pair<uint64_t, uint64_t>> ranked;  // ns, key
    for (const auto& [key, total] : pass_totals_) ranked.emplace_back(total.ns, key);
    std::sort(ranked.rbegin(), ranked.rend());
    std::string line = fmt::format("Renut: Plume GPU {} ms over {} swaps, by pass:",
                                   gpu_ns_ / 1000000, swaps_ - reported_guest_swaps_);
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 8); ++i) {
      const PassTotal& total = pass_totals_[ranked[i].second];
      const uint64_t key = ranked[i].second;
      line += fmt::format(" [{}{}x{} c{}{} {}ms {}% {}passes {}draws]",
                          (key >> 62) == 1 ? "entre-passes " : "",
                          uint32_t((key >> 32) & 0x3FFFFFFF),
                          uint32_t((key >> 8) & 0xFFFFFF), uint32_t((key >> 1) & 0x7F),
                          (key & 1) ? "+d" : "", total.ns / 1000000, 100 * total.ns / gpu_ns_,
                          total.passes, total.draws);
    }
    REXLOG_INFO("{}", line);
    pass_totals_.clear();
    gpu_ns_ = 0;
  }
  if (stats_.pipelines || stats_.lookups_missed) {
    uint64_t found = 0, compiled = 0;
    PlumePipelineCacheCounts(&found, &compiled);
    REXLOG_INFO("Renut: Plume new pipelines: {} built ahead, {} found in the cache at the draw ({} us per lookup, "
                "{} missed), {} found by a worker, {} compiled ({} ms each, {} after waiting for a sibling's "
                "compile); since launch the driver's cache had {} and compiled {}; draws used {} programs, {} of "
                "them built ahead",
                stats_.pipelines_prebuilt, stats_.pipelines_found_here,
                stats_.lookup_us / std::max<uint64_t>(1, stats_.pipelines_found_here + stats_.lookups_missed),
                stats_.lookups_missed, stats_.pipelines_found_ahead, stats_.pipelines_compiled,
                stats_.compile_us / 1000 / std::max<uint64_t>(1, stats_.pipelines_compiled),
                stats_.pipelines_waited, found, compiled, g_precompile.draw_programs.load(),
                g_precompile.draw_programs_ahead.load());
  }
  SavePlumePipelineCache();
  // A gap in the sound is a frame the output had to fill with silence; each is
  // 256 samples, 5.3 ms at 48 kHz.
  const uint64_t silent = renut_audio_silent_frames();
  if (silent != reported_silent_frames_) {
    REXLOG_INFO("Renut: audio played {} frames of silence ({} ms) since the last report",
                silent - reported_silent_frames_, (silent - reported_silent_frames_) * 16 / 3);
    reported_silent_frames_ = silent;
  }
  for (const auto& [hash, count] : missed_draws_) {
    REXLOG_INFO("Renut: Plume   missing VS {:016X}: {} draws", hash, count);
  }
  missed_draws_.clear();
  if (!drops_.empty()) {
    std::map<std::string, uint64_t> by_reason;
    std::vector<std::pair<uint64_t, std::tuple<std::string, uint64_t, uint64_t, bool>>> ranked;
    for (const auto& [key, count] : drops_) {
      by_reason[std::get<0>(key)] += count;
      ranked.emplace_back(count, key);
    }
    std::string line = "Renut: Plume drops:";
    for (const auto& [reason, count] : by_reason) line += fmt::format(" [{}] {}", reason, count);
    REXLOG_INFO("{}", line);
    std::sort(ranked.rbegin(), ranked.rend());
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 12); ++i) {
      const auto& [count, key] = ranked[i];
      REXLOG_INFO("Renut: Plume   drop [{}] vs={:016X} ps={:016X}{}: {}", std::get<0>(key), std::get<1>(key),
                  std::get<2>(key), std::get<3>(key) ? " memexport" : "", count);
    }
    drops_.clear();
  }
  // One line a report in <files>/plume-hiz.txt whatever the log level is, for
  // A/B runs that cannot read the log.
  if (!miss_dump_directory_.empty()) {
    const std::string path = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/plume-hiz.txt";
    if (FILE* out = std::fopen(path.c_str(), "a")) {
      std::fprintf(out,
                   "mode=%u swaps=%llu draws=%llu drawn=%llu dropped=%llu color_seen=%llu color_boxed=%llu "
                   "color_frustum=%llu depth_seen=%llu depth_boxed=%llu depth_frustum=%llu windows_tested=%llu "
                   "tested=%llu held=%llu answered=%llu hidden=%llu predicated=%llu test_failed=%llu cp_cpu_ms=%llu "
                   "cull_ms=%llu model_ms=%llu box_ms=%llu eval_ms=%llu memo_hits=%llu pipeline_binds=%llu nm_color=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu nm_depth=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu skipped=%llu,%llu skip=%d tests_ms=%llu\n",
                   hiz_mode_, (unsigned long long)swaps_, (unsigned long long)stats_.draws,
                   (unsigned long long)stats_.drawn, (unsigned long long)stats_.hiz_culled_total,
                   (unsigned long long)stats_.hiz_seen[1], (unsigned long long)stats_.hiz_eligible[1],
                   (unsigned long long)stats_.hiz_frustum[1], (unsigned long long)stats_.hiz_seen[0],
                   (unsigned long long)stats_.hiz_eligible[0], (unsigned long long)stats_.hiz_frustum[0],
                   (unsigned long long)stats_.hiz_windows_tested, (unsigned long long)stats_.hiz_records,
                   (unsigned long long)stats_.hiz_held, (unsigned long long)stats_.hiz_answered,
                   (unsigned long long)stats_.hiz_occluded, (unsigned long long)stats_.hiz_predicated_draws,
                   (unsigned long long)stats_.hiz_test_failed, (unsigned long long)(stats_.cp_cpu_us / 1000),
                   (unsigned long long)(stats_.hiz_cull_ns / 1000000), (unsigned long long)(stats_.hiz_stage_ns[0] / 1000000),
                   (unsigned long long)(stats_.hiz_stage_ns[1] / 1000000), (unsigned long long)(stats_.hiz_stage_ns[2] / 1000000),
                   (unsigned long long)stats_.stream_memo_hits, (unsigned long long)stats_.pipeline_binds,
                   (unsigned long long)stats_.hiz_why_model[1][1], (unsigned long long)stats_.hiz_why_model[1][2], (unsigned long long)stats_.hiz_why_model[1][3], (unsigned long long)stats_.hiz_why_model[1][4], (unsigned long long)stats_.hiz_why_model[1][5], (unsigned long long)stats_.hiz_why_model[1][6], (unsigned long long)stats_.hiz_why_model[1][7], (unsigned long long)stats_.hiz_why_model[1][8], (unsigned long long)stats_.hiz_why_model[1][9], (unsigned long long)stats_.hiz_why_model[1][10], (unsigned long long)stats_.hiz_why_model[0][1], (unsigned long long)stats_.hiz_why_model[0][2], (unsigned long long)stats_.hiz_why_model[0][3], (unsigned long long)stats_.hiz_why_model[0][4], (unsigned long long)stats_.hiz_why_model[0][5], (unsigned long long)stats_.hiz_why_model[0][6], (unsigned long long)stats_.hiz_why_model[0][7], (unsigned long long)stats_.hiz_why_model[0][8], (unsigned long long)stats_.hiz_why_model[0][9], (unsigned long long)stats_.hiz_why_model[0][10],
                   (unsigned long long)stats_.hiz_skipped[1], (unsigned long long)stats_.hiz_skipped[0], hiz_skip_ ? 1 : 0,
                   (unsigned long long)(stats_.hiz_stage_ns[3] / 1000000));
      std::fclose(out);
    }
  }
  reported_guest_swaps_ = renut_guest_swap_count();
  stats_ = {};
}

}  // namespace renut::gpu
