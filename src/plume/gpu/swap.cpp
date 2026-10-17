// IssueSwap: the guest frame's present.
#include "internal.h"

namespace renut::gpu {

void Swap::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  auto* renderer = PlumeRenderer::Get();
  ExecutorBusy busy;
  if (native_on_) {
    // Everything the lists hold for this frame, before it is shown (from the
    // ring's swap; a present the lists hold has run what came before it).
    if (!backend.lists.native_presenting_) backend.lists.RunNative(UINT32_MAX);
    backend.targets.EvictSurfaceTargets();
    native_d3d::NativeReport(backend.swaps_, backend.lists.native_draws_, backend.lists.native_bad_,
                             [this] { return backend.targets.SurfaceReport(); });
  }
  backend.recorder.FlushDeferred();
  if (skin_log_ && backend.swaps_ == 930) backend.resolve.LogSkinned();
  if (backend.draw.validate_windows_ && backend.swaps_ % 60 == 0) {
    REXLOG_INFO("Renut: Plume validation at swap {}: {} ranges compared, {} stale", backend.swaps_, backend.resolve.validate_ranges_, backend.resolve.validate_stale_);
  }
  // Before anything is picked for presenting: the one that is moves on to
  // COPY_SOURCE below, which is a transition of its own.
  backend.resolve.FlushImagesForNextFrame();
  RenderTexture* source = nullptr;
  Resolved* found = backend.draw.FindResolved(frontbuffer_ptr);
  if (resolve_log_) {
    backend.resolve.LogResolveLine(fmt::format("f={} swap front={:08X} {}x{} found={}", backend.swaps_, frontbuffer_ptr, frontbuffer_width,
                               frontbuffer_height, found != nullptr));
  }
  if (found && found->texture) {
    Resolved& r = *found;
    backend.targets.Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
    source = r.texture.get();
    frontbuffer_width = std::min(frontbuffer_width, r.width);
    frontbuffer_height = std::min(frontbuffer_height, r.height);
  }
  bool snap_dump = false;
  if (snap_taken_ && backend.resolve.snap_) {
    backend.targets.Transition(backend.resolve.snap_.get(), backend.resolve.snap_layout_, RenderTextureLayout::COPY_SOURCE);
    source = backend.resolve.snap_.get();
    frontbuffer_width = std::min(frontbuffer_width, snap_width_);
    frontbuffer_height = std::min(frontbuffer_height, snap_height_);
    const std::string root = backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/'));
    if (!backend.miss_dump_directory_.empty() && access((root + "/plume-snap-dump").c_str(), F_OK) == 0) {
      snap_dump_row_ = (snap_width_ + 63) & ~63u;
      snap_dump_ = backend.device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(snap_dump_row_) * snap_height_ * 4));
      if (snap_dump_) {
        CopyTextureToBuffer(backend.recorder.L(), snap_dump_.get(), backend.resolve.snap_.get(), snap_width_, snap_height_, snap_dump_row_, 0, 0,
                            nullptr);
        snap_dump = true;
      }
    }
  }
  snap_taken_ = false;
  snap_seen_ = 0;
  frame_draw_ = 0;
  // A traced frame also copies every exported range aside, written out below
  // once the frame has run, next to the guest bytes it started from.
  std::unique_ptr<RenderBuffer> export_dump;
  std::vector<std::pair<uint64_t, const ExportRange*>> export_dump_at;
  if (backend.draw.trace_ && !backend.streams.exports_.empty()) {
    uint64_t total = 0;
    for (const auto& [key, range] : backend.streams.exports_) total += range.size;
    if (total <= (16u << 20)) export_dump = backend.device_->createBuffer(RenderBufferDesc::ReadbackBuffer(total));
    if (export_dump) {
      backend.streams.ExportBarrier();
      uint64_t at = 0;
      for (const auto& [key, range] : backend.streams.exports_) {
        backend.recorder.L()->copyBufferRegion(RenderBufferReference(export_dump.get(), at),
                                RenderBufferReference(range.buffer.get(), 0), range.size);
        export_dump_at.emplace_back(at, &range);
        at += range.size;
      }
    }
  }
  backend.streams.RecordWritebacks();
  // FlushImagesForNextFrame already closed any open pass with its barriers.
  backend.profiling.WriteTimestamp();
  backend.profiling.timestamp_count_[backend.slot_] = backend.profiling.timestamps_used_;
  backend.profiling.pair_count_[backend.slot_] = backend.profiling.pair_used_;
  backend.profiling.pair_pending_[backend.slot_] = backend.targets.pair_capture_;
  backend.targets.pair_capture_ = false;
  backend.recorder.L()->end();
  backend.recorder.list_open_ = false;
  const auto submit_start = std::chrono::steady_clock::now();
  if (renderer) {
    renderer->SubmitAndPresent(backend.targets.list_, source, frontbuffer_width, frontbuffer_height, backend.slot_);
    if (writeback_ && !writeback_fast_ && !backend.streams.writebacks_[backend.slot_].empty()) {
      renderer->WaitForSlot(backend.slot_);
      backend.streams.ApplyWritebacks(backend.slot_);
    }
  }
  const auto submit_end = std::chrono::steady_clock::now();
  backend.stats_.submit_us +=
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count());
  if (frame_start_.time_since_epoch().count()) {
    backend.stats_.frame_us += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(submit_end - frame_start_).count());
  }
  frame_start_ = submit_end;
  // The frame is on its way: hold the command thread, and with it the game
  // behind its fences, to the rate the player chose. While the vblank holds the
  // game (its own 30 FPS and an interval that waits for it) that is its job, and
  // unlimited has nothing to wait for; both make this a no-op.
  {
    static pacing::Pacer pacer;
    pacer.Wait(pacing::VblankPaces() ? 0 : pacing::Target());
  }
  if (snap_dump && renderer) {
    renderer->WaitForAllSlots();
    const std::string root = backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/'));
    if (FILE* file = std::fopen((root + "/plume-snap.rgba").c_str(), "wb")) {
      std::fprintf(file, "%u %u %u %u\n", snap_width_, snap_height_, snap_dump_row_, uint32_t(snap_format_));
      std::fwrite(snap_dump_->map(), 4, size_t(snap_dump_row_) * snap_height_, file);
      snap_dump_->unmap();
      std::fclose(file);
    }
    std::remove((root + "/plume-snap-dump").c_str());
    if (snap_shader_) {
      std::remove((root + "/plume-snap-shader").c_str());
      snap_shader_ = 0;
    }
    snap_dump_.reset();
    REXLOG_INFO("Renut: Plume snapshot written ({}x{}, format {})", snap_width_, snap_height_, uint32_t(snap_format_));
  }
  if (depth_dump_ && renderer) {
    renderer->WaitForAllSlots();
    const std::string root = backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/'));
    const uint32_t bytes = depth_dump_format_ == uint32_t(RenderFormat::R32G32_FLOAT) ? 8 : 4;
    if (FILE* file = std::fopen((root + "/plume-depth.f32").c_str(), "wb")) {
      std::fprintf(file, "%u %u %u %u %llu %llu\n", depth_dump_w_, depth_dump_h_, depth_dump_row_, bytes,
                   (unsigned long long)depth_dump_generation_, (unsigned long long)depth_dump_frame_);
      std::fwrite(depth_dump_->map(), 1, size_t(depth_dump_row_) * depth_dump_h_ * bytes, file);
      depth_dump_->unmap();
      std::fclose(file);
    }
    std::remove((root + "/plume-depth-dump").c_str());
    depth_dump_ps_ = 0;
    depth_dump_.reset();
    REXLOG_INFO("Renut: Plume depth resolve written ({}x{}, {} bytes a texel)", depth_dump_w_, depth_dump_h_, bytes);
  }
  const uint64_t cpu_now = ThreadCpuMicroseconds();
  if (cpu_at_swap_) backend.stats_.cp_cpu_us += cpu_now - cpu_at_swap_;
  cpu_at_swap_ = cpu_now;
  if (backend.draw.trace_) {
    backend.draw.Trace("swap front={:08X} {}x{} found={}", frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
          source != nullptr);
    for (auto& [key, target] : backend.targets.targets_) {
      backend.draw.Trace("  target base={:X} fmt={} depth={} {}x{}", key & 0xFFF, (key >> 12) & 0xF, (key >> 16) & 1,
            target.width, target.height);
    }
    for (const auto& [key, r] : backend.targets.resolved_) {
      backend.draw.Trace("  resolved {:08X} host={}x{} f{} guest={}x{}/{} gen={} frame={}", uint32_t(key),
            r.width, r.height, uint32_t(r.format), r.guest_pitch, r.guest_height, r.guest_bpp,
            r.generation, r.written_frame);
    }
    for (const TracedStream& t : traced_streams_) {
      const auto* now = backend.memory_->TranslatePhysical<const uint8_t*>(t.address);
      uint32_t changed = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < t.bytes.size(); i += 4) {
        if (std::memcmp(now + i, t.bytes.data() + i, 4)) {
          ++changed;
          first = std::min(first, i);
        }
      }
      if (changed) {
        backend.draw.Trace("  stale draw #{} stream{} {:08X}+{:X}: {} of {} dwords changed since, first at +{:X}", t.draw,
              t.stream, t.address, t.bytes.size(), changed, t.bytes.size() / 4, first);
      }
    }
    traced_streams_.clear();
    std::fclose(backend.draw.trace_);
    backend.draw.trace_ = nullptr;
    traced_cascade_ps_.clear();
    REXLOG_INFO("Renut: Plume frame trace written");
    if (export_dump && renderer && !backend.miss_dump_directory_.empty()) {
      renderer->WaitForAllSlots();
      const std::string directory =
          backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/')) + "/" + trace_dump_name_;
      renutMakeDirectory(directory);
      const auto* data = static_cast<const uint8_t*>(export_dump->map());
      for (const auto& [at, range] : export_dump_at) {
        const std::string name = fmt::format("{}/{:08X}", directory, range->base);
        if (FILE* file = std::fopen((name + ".gpu").c_str(), "wb")) {
          std::fwrite(data + at, 1, range->size, file);
          std::fclose(file);
        }
        if (FILE* file = std::fopen((name + ".guest").c_str(), "wb")) {
          std::fwrite(backend.memory_->TranslatePhysical<const uint8_t*>(range->base), 1, range->size, file);
          std::fclose(file);
        }
      }
      export_dump->unmap();
      REXLOG_INFO("Renut: Plume wrote {} exported ranges", export_dump_at.size());
    }
  }
  ++backend.swaps_;
  if (g_predict.queued.load(std::memory_order_relaxed)) backend.pipelines.PredictPipelines();
  if (!backend.draw.pending_pipelines_.empty()) backend.pipelines.ReapPipelines();
  backend.draw.lookup_spent_us_ = 0;
  pipeline_frame_.store(backend.swaps_, std::memory_order_relaxed);
  // Every switch is a cvar, read here each frame (no file is touched): a change
  // takes effect at the next swap.
  {
    const int defer = std::clamp(REXCVAR_GET(renut_plume_defer_draws), 0, 2);
    if (uint32_t(defer) != defer_mode_) {
      REXLOG_INFO("Renut: Plume deferred draws mode {} (0 off, 1 depth-only, 2 opaque colour too)", defer);
    }
    defer_mode_ = uint32_t(defer);

    const bool shader_cache = REXCVAR_GET(renut_plume_shader_cache);
    if (shader_cache != shader_cache_) {
      REXLOG_INFO("Renut: Plume shader cache {}", shader_cache ? "on" : "off");
      backend.streams.shader_watch_.clear();
      backend.shaders.shader_watch_memo_.Clear();
      backend.streams.shader_block_index_.clear();
    }
    shader_cache_ = shader_cache;
    const bool key_reuse = REXCVAR_GET(renut_plume_key_reuse);
    if (key_reuse != key_reuse_) {
      REXLOG_INFO("Renut: Plume pipeline key reuse {}", key_reuse ? "on" : "off");
      // Keys made the other way name different pipelines: the memos go, the
      // pipelines are found again (or made) under the new keys.
      backend.draw.pipeline_memo_.Clear();
    }
    key_reuse_ = key_reuse;

    const int hiz = std::clamp(REXCVAR_GET(renut_plume_hiz), 0, 4);
    if (uint32_t(hiz) != backend.report.hiz_mode_) {
      REXLOG_INFO("Renut: Plume Hi-Z mode {} (0 off, 1 counts, 2 frustum/scissor, 3 + occlusion, 4 + occlusion of depth-only draws)", hiz);
    }
    backend.report.hiz_mode_ = uint32_t(hiz);

    const int fetch_spec = std::clamp(REXCVAR_GET(renut_plume_fetch_spec), 0, 1);
    if (uint32_t(fetch_spec) != fetch_spec_mode_) {
      REXLOG_INFO("Renut: Plume live fetch table as specialization constants {}", fetch_spec ? "on" : "off");
    }
    fetch_spec_mode_ = uint32_t(fetch_spec);

    const int bc = std::clamp(REXCVAR_GET(renut_plume_bc_textures), 0, 1);
    if (uint32_t(bc) != bc_mode_) {
      REXLOG_INFO("Renut: Plume BC textures {}: the 2D guest textures are uploaded again", bc ? "on" : "off");
      bc_mode_ = uint32_t(bc);
      // Every 2D texture is rebuilt in the other form the next time it is drawn.
      for (auto& [key, texture] : backend.targets.guest_textures_) backend.targets.RetireGuestTexture(texture);
      backend.targets.guest_textures_.clear();
      backend.textures.texture_memo_.Clear();
      ++backend.targets.object_epoch_;
    }

    const int quality = std::clamp(REXCVAR_GET(renut_plume_texture_quality), 0, 2);
    if (uint32_t(quality) != backend.texture_quality_) {
      REXLOG_INFO("Renut: Plume texture quality {} (each texture is uploaded again when it is next drawn)",
                  quality == 0 ? "full" : quality == 1 ? "half" : "quarter");
      backend.texture_quality_ = uint32_t(quality);
    }

    const int rel = std::clamp(REXCVAR_GET(renut_plume_rel_constants), 0, 2);
    if (uint32_t(rel) != backend.draw.rel_constants_mode_) {
      REXLOG_INFO("Renut: Plume relative vertex constants mode {} (0 four blocks, 1 one block, 2 one block loaded)", rel);
    }
    backend.draw.rel_constants_mode_ = uint32_t(rel);

    const int depth_strip = std::clamp(REXCVAR_GET(renut_plume_depth_strip), 0, 1);
    if (uint32_t(depth_strip) != depth_strip_mode_) {
      REXLOG_INFO("Renut: Plume depth-only draws without vertex outputs {}", depth_strip ? "on" : "off");
    }
    depth_strip_mode_ = uint32_t(depth_strip);

    const int adaptive = std::clamp(REXCVAR_GET(renut_plume_adaptive_patches), 0, 1);
    if (uint32_t(adaptive) != adaptive_mode_) {
      REXLOG_INFO("Renut: Plume adaptive patches {}", adaptive ? "on" : "off");
    }
    adaptive_mode_ = uint32_t(adaptive);

    const uint32_t percent = uint32_t(std::clamp(REXCVAR_GET(renut_plume_water_tess_percent), 10, 100));
    if (percent != patch_level_percent_) REXLOG_INFO("Renut: Plume water tessellation at {}%", percent);
    patch_level_percent_ = percent;

    const uint32_t minimum =
        backend.no_shadows_ ? UINT32_MAX : uint32_t(std::max(REXCVAR_GET(renut_plume_shadow_min_indices), 0));
    if (minimum != shadow_min_indices_) REXLOG_INFO("Renut: Plume shadow casters under {} indices dropped", minimum);
    shadow_min_indices_ = minimum;
  }
  // A range nothing has exported into for a while goes back to guest memory.
  // Emitters export every frame they are alive, so a couple of seconds is
  // plenty. At 600 frames a range outlived the level that exported into it:
  // the next level reused the memory for ordinary meshes, and every block it
  // wrote with the bytes the range's snapshot already held kept the old
  // level's exported vertices - triangles across the screen after a few
  // trips between Showdown Town and Nutty Acres.
  for (auto it = backend.streams.exports_.begin(); it != backend.streams.exports_.end();) {
    if (it->second.exported_frame + 30 < backend.swaps_) {
      backend.streams.retired_buffers_[backend.slot_].push_back(std::move(it->second.buffer));
      ++backend.draw.export_epoch_;
      it = backend.streams.exports_.erase(it);
    } else {
      ++it;
    }
  }
  if (backend.swaps_ % 300 == 1) backend.report.Report();
  if (backend.swaps_ % 15 == 3) backend.report.ReportMemory();
  backend.targets.BeginFrame();
}

}  // namespace renut::gpu
