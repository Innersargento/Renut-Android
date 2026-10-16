// IssueSwap: the guest frame's present.
#include "internal.h"

namespace renut::gpu {

void GpuBackend::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  auto* renderer = PlumeRenderer::Get();
  ExecutorBusy busy;
  if (native_on_) {
    // Everything the lists hold for this frame, before it is shown (from the
    // ring's swap; a present the lists hold has run what came before it).
    if (!native_presenting_) RunNative(UINT32_MAX);
    EvictSurfaceTargets();
    native_d3d::NativeReport(swaps_, native_draws_, native_bad_,
                             [this] { return SurfaceReport(); });
  }
  FlushDeferred();
  if (skin_log_ && swaps_ == 930) LogSkinned();
  if (validate_windows_ && swaps_ % 60 == 0) {
    REXLOG_INFO("Renut: Plume validation at swap {}: {} ranges compared, {} stale", swaps_, validate_ranges_, validate_stale_);
  }
  // Before anything is picked for presenting: the one that is moves on to
  // COPY_SOURCE below, which is a transition of its own.
  FlushImagesForNextFrame();
  RenderTexture* source = nullptr;
  Resolved* found = FindResolved(frontbuffer_ptr);
  if (resolve_log_) {
    LogResolveLine(fmt::format("f={} swap front={:08X} {}x{} found={}", swaps_, frontbuffer_ptr, frontbuffer_width,
                               frontbuffer_height, found != nullptr));
  }
  if (found && found->texture) {
    Resolved& r = *found;
    Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
    source = r.texture.get();
    frontbuffer_width = std::min(frontbuffer_width, r.width);
    frontbuffer_height = std::min(frontbuffer_height, r.height);
  }
  bool snap_dump = false;
  if (snap_taken_ && snap_) {
    Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_SOURCE);
    source = snap_.get();
    frontbuffer_width = std::min(frontbuffer_width, snap_width_);
    frontbuffer_height = std::min(frontbuffer_height, snap_height_);
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (!miss_dump_directory_.empty() && access((root + "/plume-snap-dump").c_str(), F_OK) == 0) {
      snap_dump_row_ = (snap_width_ + 63) & ~63u;
      snap_dump_ = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(snap_dump_row_) * snap_height_ * 4));
      if (snap_dump_) {
        CopyTextureToBuffer(L(), snap_dump_.get(), snap_.get(), snap_width_, snap_height_, snap_dump_row_, 0, 0,
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
  if (trace_ && !exports_.empty()) {
    uint64_t total = 0;
    for (const auto& [key, range] : exports_) total += range.size;
    if (total <= (16u << 20)) export_dump = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(total));
    if (export_dump) {
      ExportBarrier();
      uint64_t at = 0;
      for (const auto& [key, range] : exports_) {
        L()->copyBufferRegion(RenderBufferReference(export_dump.get(), at),
                                RenderBufferReference(range.buffer.get(), 0), range.size);
        export_dump_at.emplace_back(at, &range);
        at += range.size;
      }
    }
  }
  RecordWritebacks();
  // FlushImagesForNextFrame already closed any open pass with its barriers.
  WriteTimestamp();
  timestamp_count_[slot_] = timestamps_used_;
  pair_count_[slot_] = pair_used_;
  pair_pending_[slot_] = pair_capture_;
  pair_capture_ = false;
  L()->end();
  list_open_ = false;
  const auto submit_start = std::chrono::steady_clock::now();
  if (renderer) {
    renderer->SubmitAndPresent(list_, source, frontbuffer_width, frontbuffer_height, slot_);
    if (writeback_ && !writeback_fast_ && !writebacks_[slot_].empty()) {
      renderer->WaitForSlot(slot_);
      ApplyWritebacks(slot_);
    }
  }
  const auto submit_end = std::chrono::steady_clock::now();
  stats_.submit_us +=
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count());
  if (frame_start_.time_since_epoch().count()) {
    stats_.frame_us += uint64_t(
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
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
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
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
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
  if (cpu_at_swap_) stats_.cp_cpu_us += cpu_now - cpu_at_swap_;
  cpu_at_swap_ = cpu_now;
  if (trace_) {
    Trace("swap front={:08X} {}x{} found={}", frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
          source != nullptr);
    for (auto& [key, target] : targets_) {
      Trace("  target base={:X} fmt={} depth={} {}x{}", key & 0xFFF, (key >> 12) & 0xF, (key >> 16) & 1,
            target.width, target.height);
    }
    for (const auto& [key, r] : resolved_) {
      Trace("  resolved {:08X} host={}x{} f{} guest={}x{}/{} gen={} frame={}", uint32_t(key),
            r.width, r.height, uint32_t(r.format), r.guest_pitch, r.guest_height, r.guest_bpp,
            r.generation, r.written_frame);
    }
    for (const TracedStream& t : traced_streams_) {
      const auto* now = memory_->TranslatePhysical<const uint8_t*>(t.address);
      uint32_t changed = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < t.bytes.size(); i += 4) {
        if (std::memcmp(now + i, t.bytes.data() + i, 4)) {
          ++changed;
          first = std::min(first, i);
        }
      }
      if (changed) {
        Trace("  stale draw #{} stream{} {:08X}+{:X}: {} of {} dwords changed since, first at +{:X}", t.draw,
              t.stream, t.address, t.bytes.size(), changed, t.bytes.size() / 4, first);
      }
    }
    traced_streams_.clear();
    std::fclose(trace_);
    trace_ = nullptr;
    traced_cascade_ps_.clear();
    REXLOG_INFO("Renut: Plume frame trace written");
    if (export_dump && renderer && !miss_dump_directory_.empty()) {
      renderer->WaitForAllSlots();
      const std::string directory =
          miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/" + trace_dump_name_;
      renutMakeDirectory(directory);
      const auto* data = static_cast<const uint8_t*>(export_dump->map());
      for (const auto& [at, range] : export_dump_at) {
        const std::string name = fmt::format("{}/{:08X}", directory, range->base);
        if (FILE* file = std::fopen((name + ".gpu").c_str(), "wb")) {
          std::fwrite(data + at, 1, range->size, file);
          std::fclose(file);
        }
        if (FILE* file = std::fopen((name + ".guest").c_str(), "wb")) {
          std::fwrite(memory_->TranslatePhysical<const uint8_t*>(range->base), 1, range->size, file);
          std::fclose(file);
        }
      }
      export_dump->unmap();
      REXLOG_INFO("Renut: Plume wrote {} exported ranges", export_dump_at.size());
    }
  }
  ++swaps_;
  if (g_predict.queued.load(std::memory_order_relaxed)) PredictPipelines();
  if (!pending_pipelines_.empty()) ReapPipelines();
  lookup_spent_us_ = 0;
  pipeline_frame_.store(swaps_, std::memory_order_relaxed);
  if (!miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (swaps_ % 30 == 0) {
      const bool arm = access((root + "/plume-trace-arm").c_str(), F_OK) == 0;
      if (arm && !arm_) {
        arm_pairs_.clear();
        arm_target_ = 0;
        if (FILE* file = std::fopen((root + "/plume-trace-arm").c_str(), "r")) {
          unsigned long long target = 0;
          if (std::fscanf(file, "%llx", &target) == 1) arm_target_ = target;
          std::fclose(file);
        }
        arm_learn_ = arm_target_ ? 0 : 150;
        arm_burst_ = arm_left_ = 0;
        if (arm_target_) {
          REXLOG_INFO("Renut: Plume trace armed on shader {:016X}", arm_target_);
        } else {
          REXLOG_INFO("Renut: Plume trace armed, learning the scene's shaders");
        }
      }
      arm_ = arm;
    }
    if (arm_learn_ && !--arm_learn_) REXLOG_INFO("Renut: Plume trace armed with {} shader pairs", arm_pairs_.size());
    if (arm_ && !trace_ && !arm_left_ && !arm_new_.empty() && arm_burst_ < 6) {
      ++arm_burst_;
      arm_left_ = 3;
      for (const auto& [vs_hash, ps_hash] : arm_new_) {
        REXLOG_INFO("Renut: Plume trace burst {} on new pair vs={:016X} ps={:016X}", arm_burst_, vs_hash,
                    ps_hash);
      }
    }
    arm_new_.clear();
    if (arm_left_ && !trace_) {
      const std::string name = fmt::format("{}/plume-trace-arm-{}-{}.txt", root, arm_burst_, 4 - arm_left_);
      trace_ = std::fopen(name.c_str(), "w");
      trace_dump_name_ = fmt::format("exportdump-arm-{}-{}", arm_burst_, 4 - arm_left_);
      --arm_left_;
    }
  }
  if (!miss_dump_directory_.empty() && !trace_) {
    // Every frame: a request made when something looks wrong has to catch the
    // frame that shows it.
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    if (std::remove((root + "/plume-trace-request").c_str()) == 0) {
      trace_ = std::fopen((root + "/plume-trace.txt").c_str(), "w");
      trace_dump_name_ = "exportdump";
    }
  }
  if (swaps_ % 30 == 0 && !miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    texdump_directory_.clear();
    if (std::remove((root + "/plume-texdump-request").c_str()) == 0) {
      texdump_directory_ = root + "/texdump";
      renutMakeDirectory(texdump_directory_);
      dumped_.clear();
      REXLOG_INFO("Renut: Plume dumping the next frame's textures");
    }
  }
  if (swaps_ % 30 == 0 && !miss_dump_directory_.empty()) {
    const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
    uint32_t mask = 0;
    uint32_t bind_skip = 0;
    bool shadow_native_constants = true;
    if (FILE* file = std::fopen((root + "/plume-shadow-constants").c_str(), "r")) {
      int value = 1;
      if (std::fscanf(file, "%d", &value) == 1) shadow_native_constants = value != 0;
      std::fclose(file);
    }
    if (shadow_native_constants != shadow_native_constants_) {
      REXLOG_INFO("Renut: Plume shadow PS constants {}", shadow_native_constants ? "native skip" : "legacy upload");
    }
    shadow_native_constants_ = shadow_native_constants;
    if (FILE* file = std::fopen((root + "/plume-bind").c_str(), "r")) {
      if (std::fscanf(file, "%u", &bind_skip) != 1) bind_skip = 0;
      std::fclose(file);
    }
    if (bind_skip != bind_skip_) REXLOG_INFO("Renut: Plume bind skip {}", bind_skip);
    bind_skip_ = bind_skip;
    {
      int defer = std::clamp(REXCVAR_GET(renut_plume_defer_draws), 0, 2);
      if (FILE* file = std::fopen((root + "/plume-defer").c_str(), "r")) {
        int value = defer;
        if (std::fscanf(file, "%d", &value) == 1) defer = std::clamp(value, 0, 2);
        std::fclose(file);
      }
      if (uint32_t(defer) != defer_mode_) {
        REXLOG_INFO("Renut: Plume deferred draws mode {} (0 off, 1 depth-only, 2 opaque colour too)", defer);
      }
      defer_mode_ = uint32_t(defer);
    }
    {
      bool trail = trail_device_default_;
      if (FILE* file = std::fopen((root + "/plume-trail").c_str(), "r")) {
        int value = trail;
        if (std::fscanf(file, "%d", &value) == 1) trail = value != 0;
        std::fclose(file);
      }
      if (trail != trail_) REXLOG_INFO("Renut: Plume draw trail {}", trail ? "on" : "off");
      trail_ = trail;
    }
    {
      // files/plume-recthread: 0 records the draws on this thread, as before; 1 (the
      // default) hands them to the recorder (see RecQueue).
      bool recorder = true;
      if (FILE* file = std::fopen((root + "/plume-recthread").c_str(), "r")) {
        int value = 1;
        if (std::fscanf(file, "%d", &value) == 1) recorder = value != 0;
        std::fclose(file);
      }
      if (recorder != rec_enabled_) {
        REXLOG_INFO("Renut: Plume recording thread {}", recorder ? "on" : "off");
        SyncRecorder();
      }
      rec_enabled_ = recorder;
    }
    {
      // Resolve textures, framebuffers and streams by what the guest bound
      // rather than by hashing it again at every draw. 0 puts the old path back.
      bool objkeys = true;
      if (FILE* file = std::fopen((root + "/plume-objkeys").c_str(), "r")) {
        int value = 1;
        if (std::fscanf(file, "%d", &value) == 1) objkeys = value != 0;
        std::fclose(file);
      }
      if (objkeys != objkeys_) {
        REXLOG_INFO("Renut: Plume object keys {}", objkeys ? "on" : "off");
        ++object_epoch_;
        ++stream_epoch_;
      }
      objkeys_ = objkeys;
    }
    {
      // files/plume-shadercache and files/plume-keyreuse: 0 puts the old path
      // back, 1 forces it on; absent leaves the cvar's choice.
      auto read_switch = [&](const char* name, bool fallback) {
        bool value = fallback;
        if (FILE* file = std::fopen((root + "/" + name).c_str(), "r")) {
          int v = fallback;
          if (std::fscanf(file, "%d", &v) == 1) value = v != 0;
          std::fclose(file);
        }
        return value;
      };
      const bool shader_cache = read_switch("plume-shadercache", REXCVAR_GET(renut_plume_shader_cache));
      if (shader_cache != shader_cache_) {
        REXLOG_INFO("Renut: Plume shader cache {}", shader_cache ? "on" : "off");
        shader_watch_.clear();
        shader_watch_memo_.Clear();
        shader_block_index_.clear();
      }
      shader_cache_ = shader_cache;
      const bool key_reuse = read_switch("plume-keyreuse", REXCVAR_GET(renut_plume_key_reuse));
      if (key_reuse != key_reuse_) {
        REXLOG_INFO("Renut: Plume pipeline key reuse {}", key_reuse ? "on" : "off");
        // Keys made the other way name different pipelines: the memos go, the
        // pipelines are found again (or made) under the new keys.
        pipeline_memo_.Clear();
      }
      key_reuse_ = key_reuse;
    }
    {
      int hiz = std::clamp(REXCVAR_GET(renut_plume_hiz), 0, 4);
      if (FILE* file = std::fopen((root + "/plume-hiz").c_str(), "r")) {
        int value = hiz;
        if (std::fscanf(file, "%d", &value) == 1) hiz = std::clamp(value, 0, 4);
        std::fclose(file);
      }
      if (uint32_t(hiz) != hiz_mode_) {
        REXLOG_INFO("Renut: Plume Hi-Z mode {} (0 off, 1 counts, 2 frustum/scissor, 3 + occlusion, 4 + occlusion of depth-only draws)", hiz);
      }
      hiz_mode_ = uint32_t(hiz);
      bool skip = true;
      if (FILE* file = std::fopen((root + "/plume-hizskip").c_str(), "r")) {
        int value = 1;
        if (std::fscanf(file, "%d", &value) == 1) skip = value != 0;
        std::fclose(file);
      }
      hiz_skip_ = skip;
    }
    {
      int fetch_spec = std::clamp(REXCVAR_GET(renut_plume_fetch_spec), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-fetchspec").c_str(), "r")) {
        int value = fetch_spec;
        if (std::fscanf(file, "%d", &value) == 1) fetch_spec = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(fetch_spec) != fetch_spec_mode_) {
        REXLOG_INFO("Renut: Plume live fetch table as specialization constants {}", fetch_spec ? "on" : "off");
      }
      fetch_spec_mode_ = uint32_t(fetch_spec);
    }
    {
      // 1 (default): a shadow map resolved whole is sampled in place, not copied.
      int alias = 1;
      if (FILE* file = std::fopen((root + "/plume-depthalias").c_str(), "r")) {
        int value = alias;
        if (std::fscanf(file, "%d", &value) == 1) alias = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if ((alias != 0) != depth_alias_) {
        REXLOG_INFO("Renut: Plume depth maps resolved {}", alias ? "in place (no copy)" : "by copy");
      }
      depth_alias_ = alias != 0;
    }
    {
      int bc = std::clamp(REXCVAR_GET(renut_plume_bc_textures), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-bc").c_str(), "r")) {
        int value = bc;
        if (std::fscanf(file, "%d", &value) == 1) bc = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(bc) != bc_mode_) {
        REXLOG_INFO("Renut: Plume BC textures {}: the 2D guest textures are uploaded again", bc ? "on" : "off");
        bc_mode_ = uint32_t(bc);
        // Every 2D texture is rebuilt in the other form the next time it is drawn.
        for (auto& [key, texture] : guest_textures_) RetireGuestTexture(texture);
        guest_textures_.clear();
        texture_memo_.Clear();
        ++object_epoch_;
      }
    }
    {
      int quality = std::clamp(REXCVAR_GET(renut_plume_texture_quality), 0, 2);
      if (FILE* file = std::fopen((root + "/plume-texquality").c_str(), "r")) {
        int value = quality;
        if (std::fscanf(file, "%d", &value) == 1) quality = std::clamp(value, 0, 2);
        std::fclose(file);
      }
      if (uint32_t(quality) != texture_quality_) {
        REXLOG_INFO("Renut: Plume texture quality {} (each texture is uploaded again when it is next drawn)",
                    quality == 0 ? "full" : quality == 1 ? "half" : "quarter");
        texture_quality_ = uint32_t(quality);
      }
    }
    {
      int rel = std::clamp(REXCVAR_GET(renut_plume_rel_constants), 0, 2);
      if (FILE* file = std::fopen((root + "/plume-relconst").c_str(), "r")) {
        int value = rel;
        if (std::fscanf(file, "%d", &value) == 1) rel = (value >= 0 && value <= 2) || value == 11 || value == 12 ? value : 0;
        std::fclose(file);
      }
      if (uint32_t(rel) != rel_constants_mode_) {
        REXLOG_INFO("Renut: Plume relative vertex constants mode {} (0 four blocks, 1 one block, 2 one block loaded, "
                    "11/12 alternate 0 with 1/2 per draw)", rel);
      }
      rel_constants_mode_ = uint32_t(rel);
    }
    {
      int depth_strip = std::clamp(REXCVAR_GET(renut_plume_depth_strip), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-depthstrip").c_str(), "r")) {
        int value = depth_strip;
        if (std::fscanf(file, "%d", &value) == 1) depth_strip = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(depth_strip) != depth_strip_mode_) {
        REXLOG_INFO("Renut: Plume depth-only draws without vertex outputs {}", depth_strip ? "on" : "off");
      }
      depth_strip_mode_ = uint32_t(depth_strip);
    }
    {
      int adaptive = std::clamp(REXCVAR_GET(renut_plume_adaptive_patches), 0, 1);
      if (FILE* file = std::fopen((root + "/plume-adaptive").c_str(), "r")) {
        int value = adaptive;
        if (std::fscanf(file, "%d", &value) == 1) adaptive = std::clamp(value, 0, 1);
        std::fclose(file);
      }
      if (uint32_t(adaptive) != adaptive_mode_) {
        REXLOG_INFO("Renut: Plume adaptive patches {}", adaptive ? "on" : "off");
      }
      adaptive_mode_ = uint32_t(adaptive);
    }
    {
      unsigned int percent = 100;
      if (FILE* file = std::fopen((root + "/plume-patchlevel").c_str(), "r")) {
        if (std::fscanf(file, "%u", &percent) != 1) percent = 100;
        std::fclose(file);
      }
      percent = std::clamp(percent, 10u, 100u);
      if (percent != patch_level_percent_) REXLOG_INFO("Renut: Plume water tessellation at {}%", percent);
      patch_level_percent_ = percent;
    }
    {
      unsigned int minimum = no_shadows_ ? UINT32_MAX : 0;
      if (!no_shadows_) if (FILE* file = std::fopen((root + "/plume-shadowmin").c_str(), "r")) {
        if (std::fscanf(file, "%u", &minimum) != 1) minimum = 0;
        std::fclose(file);
      }
      if (minimum != shadow_min_indices_) REXLOG_INFO("Renut: Plume shadow casters under {} indices dropped", minimum);
      shadow_min_indices_ = minimum;
    }
    // Arms a per-pair GPU profile of the next few frames (see PairBegin). The
    // file is removed as it is read, so one write is one capture.
    if (FILE* file = std::fopen((root + "/plume-pairprof").c_str(), "r")) {
      int mode = 1, frames = 3;
      const int read = std::fscanf(file, "%d %d", &mode, &frames);
      std::fclose(file);
      std::remove((root + "/plume-pairprof").c_str());
      if (read >= 1 && mode >= 1 && pair_pool_ != VK_NULL_HANDLE && pairprof_frames_left_ == 0) {
        pairprof_mode_ = uint32_t(std::min(mode, 2));
        pairprof_frames_left_ = uint32_t(std::clamp(read >= 2 ? frames : 3, 1, 16));
        pair_totals_.clear();
        pair_frames_ = 0;
        pair_span_ns_ = pair_gap_ns_ = pair_frame_ns_ = 0;
        REXLOG_INFO("Renut: Plume pair profile: mode {}, {} frames", pairprof_mode_, pairprof_frames_left_);
      }
    }
    {
      bool pass_all = false;
      if (FILE* file = std::fopen((root + "/plume-pred-all").c_str(), "r")) {
        int value = 0;
        if (std::fscanf(file, "%d", &value) == 1) pass_all = value != 0;
        std::fclose(file);
      }
      if (pass_all != pred_pass_all_) REXLOG_INFO("Renut: predicated packets {}", pass_all ? "all run" : "bin-checked");
      pred_pass_all_ = pass_all;
      bool fragpos = false;
      if (FILE* file = std::fopen((root + "/plume-band-fragpos").c_str(), "r")) {
        int value = 0;
        if (std::fscanf(file, "%d", &value) == 1) fragpos = value != 0;
        std::fclose(file);
      }
      if (fragpos != band_fragpos_) REXLOG_INFO("Renut: band-relative pixel positions {}", fragpos ? "on" : "off");
      band_fragpos_ = fragpos;
    }
    {
      int split_mode = 2;
      if (FILE* file = std::fopen((root + "/plume-keep-split").c_str(), "r")) {
        if (std::fscanf(file, "%d", &split_mode) != 1) split_mode = 2;
        std::fclose(file);
      }
      if (split_mode != g_renut_scene_split_mode.exchange(split_mode))
        REXLOG_INFO("Renut: scene band split mode {}", split_mode);
    }
    if (FILE* file = std::fopen((root + "/plume-skip").c_str(), "r")) {
      if (std::fscanf(file, "%u", &mask) != 1) mask = 0;
      std::fclose(file);
    }
    if (mask != skip_mask_) {
      REXLOG_INFO("Renut: Plume skip mask {:X}", mask);
      // Two of its bits change how a level's layout is worked out.
      fetch_layouts_.clear();
      layout_memo_.Clear();
      ++object_epoch_;
    }
    skip_mask_ = mask;
    const bool sync_draws = access((root + "/plume-syncdraws").c_str(), F_OK) == 0;
    if (sync_draws != sync_draws_) REXLOG_INFO("Renut: Plume synced draws {}", sync_draws ? "on" : "off");
    sync_draws_ = sync_draws;
    {
      unsigned every = 0;
      if (FILE* file = std::fopen((root + "/plume-syncevery").c_str(), "r")) {
        if (std::fscanf(file, "%u", &every) != 1) every = 0;
        std::fclose(file);
      }
      if (every != sync_every_) REXLOG_INFO("Renut: Plume list submitted and waited for every {} draws", every);
      sync_every_ = every;
    }
    {
      unsigned long long watch = 0;
      if (FILE* file = std::fopen((root + "/plume-syncwatch").c_str(), "r")) {
        if (std::fscanf(file, "%llx", &watch) != 1) watch = 0;
        std::fclose(file);
      }
      sync_watch_ = watch;
      unsigned long long delay = 0;
      unsigned mask = 15;
      if (FILE* file = std::fopen((root + "/plume-retire-delay").c_str(), "r")) {
        if (std::fscanf(file, "%llu %u", &delay, &mask) < 1) delay = 0;
        std::fclose(file);
      }
      if (delay != retire_delay_ || mask != retire_mask_) {
        REXLOG_INFO("Renut: Plume retired objects (classes {:X}) kept {} frames", mask, delay);
      }
      retire_delay_ = delay;
      retire_mask_ = mask;
    }
    const bool keep_streams = access((root + "/plume-no-persist").c_str(), F_OK) != 0;
    if (keep_streams != keep_streams_) {
      REXLOG_INFO("Renut: Plume kept streams {}", keep_streams ? "on" : "off");
      if (!keep_streams) {
        for (auto& [key, kept] : kept_streams_) {
          if (kept.buffer) retired_buffers_[slot_].push_back(std::move(kept.buffer));
        }
        ++stream_epoch_;
    kept_streams_.clear();
  kept_memo_.Clear();
        kept_bytes_ = 0;
      }
    }
    keep_streams_ = keep_streams;
    const bool writeback = REXCVAR_GET(renut_plume_memexport_readback) ||
        access((root + "/plume-writeback").c_str(), F_OK) == 0;
    if (writeback != writeback_) {
      REXLOG_INFO("Renut: Plume export writeback {}", writeback ? "on" : "off");
    }
    writeback_ = writeback;
    writeback_fast_ = REXCVAR_GET(renut_plume_memexport_fast);
    uint32_t address = 0, ordinal = 0;
    if (FILE* file = std::fopen((root + "/plume-snap").c_str(), "r")) {
      if (std::fscanf(file, "%x %u", &address, &ordinal) != 2) address = ordinal = 0;
      std::fclose(file);
    }
    if (address != snap_address_ || ordinal != snap_ordinal_) {
      REXLOG_INFO("Renut: Plume snapshot of resolve {} into {:08X}", ordinal, address);
    }
    snap_address_ = address;
    snap_ordinal_ = ordinal;
    unsigned long long snap_shader = 0;
    if (FILE* file = std::fopen((root + "/plume-snap-shader").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &snap_shader) != 1) snap_shader = 0;
      std::fclose(file);
    }
    if (snap_shader != snap_shader_) {
      REXLOG_INFO("Renut: Plume snapshot of the first resolve shader {:016X} samples", snap_shader);
    }
    snap_shader_ = snap_shader;
    resolve_log_ = access((root + "/plume-resolve-log").c_str(), F_OK) == 0;
    {
      std::vector<ConstOverride> overrides;
      if (FILE* file = std::fopen((root + "/plume-const").c_str(), "r")) {
        char stage[8];
        while (std::fscanf(file, "%7s", stage) == 1) {
          if (stage[0] == 'b') {
            unsigned int word = 0, set = 0, clear = 0;
            if (std::fscanf(file, "%u %x %x", &word, &set, &clear) != 3) break;
            if (word < 8) {
              ConstOverride o = {false, word, {}};
              o.bool_word = true;
              o.set = set;
              o.clear = clear;
              overrides.push_back(o);
            }
          } else {
            unsigned int reg;
            float v[4];
            if (std::fscanf(file, "%u %f %f %f %f", &reg, &v[0], &v[1], &v[2], &v[3]) != 5) break;
            if (reg < 256) overrides.push_back({stage[0] == 'p', reg, {v[0], v[1], v[2], v[3]}});
          }
        }
        std::fclose(file);
      }
      if (overrides.size() != const_overrides_.size()) {
        REXLOG_INFO("Renut: Plume constant overrides: {}", overrides.size());
      }
      const_overrides_ = std::move(overrides);
    }
    {
      unsigned int fix = 1;
      if (FILE* file = std::fopen((root + "/plume-refractfix").c_str(), "r")) {
        if (std::fscanf(file, "%u", &fix) != 1) fix = 1;
        std::fclose(file);
      }
      refraction_fix_ = fix != 0;
    }
    {
      unsigned int on = 0;
      float scale = 1.0f;
      if (FILE* file = std::fopen((root + "/plume-polyoffset").c_str(), "r")) {
        const int read = std::fscanf(file, "%u %f", &on, &scale);
        if (read < 1) on = 0;
        if (read < 2) scale = 1.0f;
        std::fclose(file);
      }
      if ((on != 0) != poly_offset_ || scale != poly_offset_scale_) {
        REXLOG_INFO("Renut: Plume polygon offset {} x{}", on ? "on" : "off", scale);
      }
      poly_offset_ = on != 0;
      poly_offset_scale_ = scale;
    }
    {
      unsigned int dedupe = 1;
      if (FILE* file = std::fopen((root + "/plume-dedupe").c_str(), "r")) {
        if (std::fscanf(file, "%u", &dedupe) != 1) dedupe = 1;
        std::fclose(file);
      }
      if ((dedupe != 0) != twin_dedupe_) REXLOG_INFO("Renut: Plume twin-draw dedupe {}", dedupe ? "on" : "off");
      twin_dedupe_ = dedupe != 0;
    }
    unsigned long long depth_dump_ps = 0;
    if (FILE* file = std::fopen((root + "/plume-depth-dump").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &depth_dump_ps) != 1) depth_dump_ps = 0;
      std::fclose(file);
    }
    if (depth_dump_ps != depth_dump_ps_ && !depth_dump_) {
      REXLOG_INFO("Renut: Plume depth dump of the first depth resolve shader {:016X} samples", depth_dump_ps);
    }
    if (!depth_dump_) depth_dump_ps_ = depth_dump_ps;
    uint32_t first = 0, end = 0;
    if (FILE* file = std::fopen((root + "/plume-drawskip").c_str(), "r")) {
      if (std::fscanf(file, "%u %u", &first, &end) != 2) first = end = 0;
      std::fclose(file);
    }
    if (first != drawskip_first_ || end != drawskip_end_) {
      REXLOG_INFO("Renut: Plume dropping draws {}..{} of every frame", first, end);
    }
    drawskip_first_ = first;
    drawskip_end_ = end;
    unsigned long long skip_shader = 0;
    if (FILE* file = std::fopen((root + "/plume-skip-shader").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &skip_shader) != 1) skip_shader = 0;
      std::fclose(file);
    }
    if (skip_shader != skip_shader_) REXLOG_INFO("Renut: Plume dropping shader {:016X}", skip_shader);
    skip_shader_ = skip_shader;
    {
      std::unordered_set<uint64_t> list;
      bool present = false;
      if (FILE* file = std::fopen((root + "/plume-skin-skip").c_str(), "r")) {
        present = true;
        unsigned long long hash;
        while (std::fscanf(file, "%llx", &hash) == 1) list.insert(hash);
        std::fclose(file);
      }
      std::lock_guard<std::mutex> lock(skinned_mutex_);
      if (list != skin_skip_) REXLOG_INFO("Renut: Plume dropping {} skinned vertex shaders", list.size());
      skin_skip_ = std::move(list);
      skin_log_ = present || access((root + "/plume-skin-log").c_str(), F_OK) == 0;
    }
    unsigned long long debug_shader = 0;
    uint32_t debug_mode = 7;
    if (FILE* file = std::fopen((root + "/plume-debug-shader").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &debug_shader) != 1) debug_shader = 0;
      if (std::fscanf(file, "%u", &debug_mode) != 1) debug_mode = 7;
      std::fclose(file);
    }
    debug_mode_ = debug_mode;
    unsigned int white_slots = 0;
    if (FILE* file = std::fopen((root + "/plume-white-slots").c_str(), "r")) {
      if (std::fscanf(file, "%x", &white_slots) != 1) white_slots = 0;
      std::fclose(file);
    }
    if (white_slots != white_slots_) REXLOG_INFO("Renut: Plume white slots {:08X}", white_slots);
    white_slots_ = white_slots;
    unsigned long long trace_ps = 0;
    if (FILE* file = std::fopen((root + "/plume-trace-ps").c_str(), "r")) {
      if (std::fscanf(file, "%llx", &trace_ps) != 1) trace_ps = 0;
      std::fclose(file);
    }
    trace_ps_ = trace_ps;
    unsigned int pixel_center = 0;
    if (FILE* file = std::fopen((root + "/plume-pixel-center").c_str(), "r")) {
      if (std::fscanf(file, "%u", &pixel_center) != 1) pixel_center = 0;
      std::fclose(file);
    }
    if (pixel_center != pixel_center_debug_) REXLOG_INFO("Renut: Plume pixel centre mode {}", pixel_center);
    pixel_center_debug_ = pixel_center;
    if (debug_shader != debug_shader_) REXLOG_INFO("Renut: Plume debugging shader {:016X}", debug_shader);
    debug_shader_ = debug_shader;
  }
  // A range nothing has exported into for a while goes back to guest memory.
  // Emitters export every frame they are alive, so a couple of seconds is
  // plenty. At 600 frames a range outlived the level that exported into it:
  // the next level reused the memory for ordinary meshes, and every block it
  // wrote with the bytes the range's snapshot already held kept the old
  // level's exported vertices - triangles across the screen after a few
  // trips between Showdown Town and Nutty Acres.
  for (auto it = exports_.begin(); it != exports_.end();) {
    if (it->second.exported_frame + 30 < swaps_) {
      retired_buffers_[slot_].push_back(std::move(it->second.buffer));
      ++export_epoch_;
      it = exports_.erase(it);
    } else {
      ++it;
    }
  }
  if (swaps_ % 300 == 1) Report();
  if (swaps_ % 15 == 3) ReportMemory();
  BeginFrame();
}

}  // namespace renut::gpu
