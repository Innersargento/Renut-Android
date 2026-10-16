// The draw recording thread, deferred draws and the device-lost report.
#include "internal.h"

namespace renut::gpu {

void PlumeCommandProcessor::RecQueue(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count) {
  if (!rec_worker_.thread.joinable()) {
    rec_worker_.mutex = &rec_mutex_;
    rec_worker_.cv = &rec_cv_;
    rec_worker_.stop = &rec_stop_;
    rec_stop_ = false;
    rec_worker_.thread = std::thread([this] { RecThreadMain(); });
    rec_thread_id_ = rec_worker_.thread.get_id();
  }
  if (!rec_pending_) {
    {
      std::lock_guard<std::mutex> lock(rec_mutex_);
      if (!rec_free_.empty()) {
        rec_pending_ = rec_free_.back();
        rec_free_.pop_back();
      }
    }
    if (!rec_pending_) {
      rec_owned_.push_back(std::make_unique<RecJob>());
      rec_pending_ = rec_owned_.back().get();
      rec_pending_->packets.reserve(kRecBatch);
      rec_pending_->slots.reserve(kRecBatch * 8);
    }
    rec_pending_->packets.clear();
    rec_pending_->slots.clear();
  }
  RecJob& job = *rec_pending_;
  DrawPacket& copy = job.packets.emplace_back(pk);
  copy.first_slot = uint32_t(job.slots.size());
  copy.slot_count = slot_count;
  job.slots.insert(job.slots.end(), slots, slots + slot_count);
  if (job.packets.size() >= kRecBatch) RecPost();
}

void PlumeCommandProcessor::RecPost() {
  if (!rec_pending_) return;
  if (rec_pending_->packets.empty()) return;
  {
    std::lock_guard<std::mutex> lock(rec_mutex_);
    rec_queue_.push_back(rec_pending_);
    rec_queued_.fetch_add(1, std::memory_order_release);
    rec_posted_.fetch_add(1, std::memory_order_release);
  }
  rec_pending_ = nullptr;
  rec_cv_.notify_one();
}

// Waits until everything handed to the recorder has been recorded.
void PlumeCommandProcessor::SyncRecorder() {
  if (OnRecorder()) return;
  RecPost();
  const uint64_t target = rec_posted_.load(std::memory_order_acquire);
  if (rec_completed_.load(std::memory_order_acquire) >= target) return;
  for (int i = 0; i < 4000; ++i) {
    if (rec_completed_.load(std::memory_order_acquire) >= target) return;
  }
  std::unique_lock<std::mutex> lock(rec_mutex_);
  rec_done_cv_.wait(lock, [&] { return rec_completed_.load(std::memory_order_acquire) >= target; });
}

void PlumeCommandProcessor::RecThreadMain() {
  pthread_setname_np(pthread_self(), "PlumeRecord");
  for (;;) {
    RecJob* job = nullptr;
    // A short spin first: waking a sleeping thread costs more than the gap
    // between two batches of a busy frame.
    for (int i = 0; i < 20000 && !job; ++i) {
      if (rec_queued_.load(std::memory_order_acquire) == 0) continue;
      std::lock_guard<std::mutex> lock(rec_mutex_);
      if (!rec_queue_.empty()) {
        job = rec_queue_.front();
        rec_queue_.pop_front();
        rec_queued_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    if (!job) {
      std::unique_lock<std::mutex> lock(rec_mutex_);
      rec_cv_.wait(lock, [&] { return rec_stop_ || !rec_queue_.empty(); });
      if (rec_queue_.empty()) return;  // stopping
      job = rec_queue_.front();
      rec_queue_.pop_front();
      rec_queued_.fetch_sub(1, std::memory_order_relaxed);
    }
    for (const DrawPacket& pk : job->packets) {
      EmitDraw(pk, job->slots.data() + pk.first_slot, pk.slot_count);
    }
    {
      std::lock_guard<std::mutex> lock(rec_mutex_);
      rec_free_.push_back(job);
      rec_completed_.fetch_add(1, std::memory_order_release);
    }
    rec_done_cv_.notify_all();
  }
}

// Records one packet: the pass, the pipeline and every piece of per-draw state
// that differs from what the list already has bound, then the draw itself.
void PlumeCommandProcessor::EmitDraw(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count) {
  // Not from the recorder (the Hi-Z windows, the debug modes): nothing may be
  // in flight while this thread touches the bound state.
  if (!OnRecorder()) SyncRecorder();
  const bool pair_stamped = pair_capture_ && PairBegin(pk);
  if (OnRecorder()) {
    emit_colors_ = pk.pass_colors;
    emit_depth_ = pk.pass_depth;
  } else {
    color_count_of_pass_ = pk.pass_colors;
    depth_of_pass_ = pk.pass_depth;
  }
  SetFramebuffer(pk.framebuffer);
  // plume-skip bit 262144 keeps the bound pipeline whenever it can stand in
  // for the draw's own (same attachments and vertex input): the picture is
  // wrong, and what the GPU saves is the price of switching programs.
  const bool keep_pipeline = (skip_mask_ & 262144) && bound_pipeline_ && pk.interface_key == bound_interface_key_;
  if (bound_pipeline_ != pk.pipeline && !keep_pipeline) {
    L()->setPipeline(pk.pipeline);
    bound_pipeline_ = pk.pipeline;
    bound_interface_key_ = pk.interface_key;
    ++rec_stats_.pipeline_binds;
  }
  // Dynamic state of the pipelines that use it (the Vulkan layer makes it so,
  // see plume_renderer.cpp): not part of Plume's interface, so on the command
  // buffer itself.
  if (pk.blend_constants || pk.stencil) {
    const VkCommandBuffer commands = static_cast<VulkanCommandList*>(L())->vk;
    if (pk.blend_constants) vkCmdSetBlendConstants(commands, pk.blend_factor);
    if (pk.stencil) {
      vkCmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, pk.stencil_ref);
      vkCmdSetStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, pk.stencil_mask);
      vkCmdSetStencilWriteMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, pk.stencil_write_mask);
    }
  }
  if (std::memcmp(pk.push, pushed_constants_, sizeof(pk.push)) != 0 &&
             !((bind_skip_ & 2) && pushed_constants_[0] != UINT64_MAX)) {
    L()->setGraphicsPushConstants(0, pk.push, 0, sizeof(pk.push));
    std::memcpy(pushed_constants_, pk.push, sizeof(pk.push));
  }
  if (std::memcmp(pk.constant_offsets, bound_constant_offsets_, sizeof(pk.constant_offsets)) != 0 &&
      !((bind_skip_ & 1) && bound_constant_offsets_[0] != UINT32_MAX)) {
    vkCmdBindDescriptorSets(static_cast<VulkanCommandList*>(L())->vk, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            static_cast<VulkanPipelineLayout*>(layout_.get())->vk, 4, 1, &constants_set_,
                            kConstantBindings, pk.constant_offsets);
    std::memcpy(bound_constant_offsets_, pk.constant_offsets, sizeof(pk.constant_offsets));
  }
  // Dynamic state, vertex and index buffers are only set when they change:
  // Plume passes each call straight to Vulkan, and Turnip re-emits the state
  // behind every one of them at the next draw.
  if (!viewport_bound_ ||
      (std::memcmp(&pk.viewport, &bound_viewport_, sizeof(pk.viewport)) != 0 && !(bind_skip_ & 8))) {
    L()->setViewports(&pk.viewport, 1);
    bound_viewport_ = pk.viewport;
    viewport_bound_ = true;
  }
  if (!scissor_bound_ ||
      (std::memcmp(&pk.scissor, &bound_scissor_, sizeof(pk.scissor)) != 0 && !(bind_skip_ & 8))) {
    L()->setScissors(&pk.scissor, 1);
    bound_scissor_ = pk.scissor;
    scissor_bound_ = true;
  }
  for (uint32_t i = 0; i < slot_count; ++i) {
    const RenderInputSlot& slot = slots[i].slot;
    const RenderVertexBufferView& view = slots[i].view;
    VertexBinding& bound = bound_vertex_buffers_[slot.index];
    if ((bound.buffer != view.buffer.ref || bound.offset != view.buffer.offset) &&
        !((bind_skip_ & 4) && bound.buffer)) {
      // Straight to Vulkan: Plume's wrapper goes through two thread_local
      // vectors a call, which was ~4% of this thread at a call or two a draw.
      if (const auto* vb = static_cast<const VulkanBuffer*>(view.buffer.ref)) {
        const VkDeviceSize offset = view.buffer.offset;
        vkCmdBindVertexBuffers(static_cast<VulkanCommandList*>(L())->vk, slot.index, 1, &vb->vk, &offset);
      } else {
        L()->setVertexBuffers(slot.index, &view, 1, &slot);
      }
      bound.buffer = view.buffer.ref;
      bound.offset = view.buffer.offset;
    }
  }
  // A draw the Hi-Z test answered for runs only where the answer is non-zero.
  const bool predicated = pk.hiz_pred_offset != UINT32_MAX && hiz_pred_[slot_] != nullptr &&
                          pk.kind != DrawPacket::Kind::kNone;
  VkCommandBuffer predicate_cb = VK_NULL_HANDLE;
  const bool queried = pk.hiz_query != UINT32_MAX && hiz_query_pool_ != VK_NULL_HANDLE &&
                       pk.kind != DrawPacket::Kind::kNone;
  if (queried) {
    auto* vulkan = static_cast<VulkanCommandList*>(L());
    vulkan->checkActiveRenderPass();
    predicate_cb = vulkan->vk;
    vkCmdBeginQuery(predicate_cb, hiz_query_pool_, pk.hiz_query, 0);
  }
  if (predicated) {
    auto* vulkan = static_cast<VulkanCommandList*>(L());
    vulkan->checkActiveRenderPass();
    predicate_cb = vulkan->vk;
    VkConditionalRenderingBeginInfoEXT info = {VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
    info.buffer = static_cast<VulkanBuffer*>(hiz_pred_[slot_].get())->vk;
    info.offset = pk.hiz_pred_offset;
    vkCmdBeginConditionalRenderingEXT(predicate_cb, &info);
  }
  switch (pk.kind) {
    case DrawPacket::Kind::kNone: break;
    case DrawPacket::Kind::kIndexed:
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      L()->drawIndexedInstanced(pk.count, pk.instances, pk.first, pk.base_vertex, 0);
      break;
    case DrawPacket::Kind::kPatch:
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      L()->drawIndexedInstanced(pk.count, 1, 0, 0, 0);
      break;
    case DrawPacket::Kind::kPatchIndirect: {
      BindIndexBuffer(pk.index_buffer, pk.index_format);
      auto* vulkan = static_cast<VulkanCommandList*>(L());
      vulkan->checkActiveRenderPass();
      vkCmdDrawIndexedIndirect(vulkan->vk, static_cast<VulkanBuffer*>(pk.index_buffer)->vk, pk.indirect_offset, 1,
                               sizeof(VkDrawIndexedIndirectCommand));
      break;
    }
    case DrawPacket::Kind::kPlain: L()->drawInstanced(pk.count, 1, pk.first, 0); break;
  }
  if (predicated) {
    vkCmdEndConditionalRenderingEXT(predicate_cb);
    ++rec_stats_.predicated;
  }
  if (queried) vkCmdEndQuery(predicate_cb, hiz_query_pool_, pk.hiz_query);
  if (pair_stamped) PairStamp();
  ++rec_stats_.drawn;
  if (!pass_marks_[slot_].empty()) ++pass_marks_[slot_].back().draws;
  if (trail_) {
    // Only one draw in eight asks (a hash lookup a draw was over 1% of this
    // thread): the report of a lost device calls a pair new from its first
    // frame, and an unknown one is simply not called new.
    uint64_t first_frame = 0;
    if ((rec_stats_.drawn & 7) == 0) {
      first_frame = pair_first_frame_.try_emplace(pk.vs_hash * 1099511628211ull ^ pk.ps_hash, swaps_).first->second;
    }
    const uint64_t at = recent_draw_count_.load(std::memory_order_relaxed);
    RecentDraw& recent = recent_draws_[at % kRecentDraws];
    recent = {swaps_, pk.vs_hash, pk.ps_hash, uint64_t(uintptr_t(pk.pipeline)),
              uint64_t(uintptr_t(pk.framebuffer)), pk.s0_addr, pk.s10_addr, first_frame,
              uint32_t(pk.kind), pk.count, pk.first, pk.s0_size, pk.s10_size};
    std::memcpy(recent.stream_addr, pk.stream_addr, sizeof(recent.stream_addr));
    std::memcpy(recent.stream_size, pk.stream_size, sizeof(recent.stream_size));
    recent.base_vertex = pk.base_vertex;
    auto address_of = [](const RenderBuffer* buffer) -> uint64_t {
      const auto* vulkan = static_cast<const VulkanBuffer*>(buffer);
      return vulkan && (vulkan->desc.flags & RenderBufferFlag::DEVICE_ADDRESSABLE) ? vulkan->getDeviceAddress() : 0;
    };
    if (pk.kind == DrawPacket::Kind::kIndexed || pk.kind == DrawPacket::Kind::kPatch) {
      recent.index_size = pk.index_format == RenderFormat::R32_UINT ? 4 : 2;
      recent.index_addr = address_of(pk.index_buffer);
      recent.index_bytes = pk.count * recent.index_size;
      if (pk.kind == DrawPacket::Kind::kIndexed) recent.index_addr += uint64_t(pk.first) * recent.index_size;
    }
    for (uint32_t i = 0; i < slot_count && recent.vertex_count < 6; ++i) {
      const RenderVertexBufferView& view = slots[i].view;
      recent.vertex[recent.vertex_count++] = {address_of(view.buffer.ref) + view.buffer.offset, view.size,
                                              slots[i].slot.index};
    }
    recent_draw_count_.store(at + 1, std::memory_order_release);
  }
  if (sync_draws_) {
    SyncAfterDraw(pk);
  } else if (sync_every_ > 1 && ++sync_every_counter_ >= sync_every_) {
    sync_every_counter_ = 0;
    SyncEvery();
  }
}

// A lost device answers every later call with an error and nothing in the guest
// ever learns of it: the watchdog just keeps waiting for a fence nobody will
// signal. Names the draws of the last frames, new vertex/pixel pairs first -
// the Mali takes the device down on a draw's data or program, and the draw is
// recorded a frame or two before the submit that reports it.
void PlumeCommandProcessor::ReportDeviceLost() {
  unsigned long long avail_kb = 0;
  if (FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
    char line[128];
    while (std::fgets(line, sizeof line, meminfo)) {
      if (std::sscanf(line, "MemAvailable: %llu kB", &avail_kb) == 1) break;
    }
    std::fclose(meminfo);
  }
  const uint64_t now = swaps_;
  REXLOG_ERROR("Renut: Plume device lost at swap {}; {} MB of RAM available; {} draws recorded", now,
               avail_kb >> 10, recent_draw_count_.load());
  REXLOG_ERROR("Renut: Plume 2D descriptor writes {} (skipped {} as unchanged)", descriptor_writes_,
               descriptor_writes_skipped_);
  if (validate_windows_) {
    REXLOG_ERROR("Renut: Plume validation: {} ranges compared, {} stale", validate_ranges_, validate_stale_);
  }
  if (skin_log_) LogSkinned();
  const uint64_t total = recent_draw_count_.load(std::memory_order_acquire);
  const uint64_t begin = total > kRecentDraws ? total - kRecentDraws : 0;
  // Every draw of the last two frames with the memory it reads, for matching
  // against the fault address the kernel logs (dmesg, via adb bugreport).
  if (!miss_dump_directory_.empty()) {
    const std::string path = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/')) + "/plume-lost.txt";
    if (FILE* out = std::fopen(path.c_str(), "w")) {
      std::fprintf(out, "device lost at swap %llu\n", (unsigned long long)now);
      for (uint64_t i = begin; i < total; ++i) {
        const RecentDraw& d = recent_draws_[i % kRecentDraws];
        if (d.frame + 2 < now) continue;
        std::fprintf(out, "d%llu f%llu vs=%016llX ps=%016llX kind=%u n=%u first=%u base=%d pipe=%llX fb=%llX",
                     (unsigned long long)i, (unsigned long long)d.frame, (unsigned long long)d.vs,
                     (unsigned long long)d.ps, d.kind, d.count, d.first, d.base_vertex, (unsigned long long)d.pipeline,
                     (unsigned long long)d.framebuffer);
        std::fprintf(out, " ib=%llX+%X/%u", (unsigned long long)d.index_addr, d.index_bytes, d.index_size);
        for (uint32_t v = 0; v < d.vertex_count; ++v) {
          std::fprintf(out, " vb%u=%llX+%X", d.vertex[v].slot, (unsigned long long)d.vertex[v].addr, d.vertex[v].size);
        }
        for (uint32_t st = 0; st < 17; ++st) {
          if (d.stream_addr[st]) {
            std::fprintf(out, " s%u=%llX+%X", st, (unsigned long long)d.stream_addr[st], d.stream_size[st]);
          }
        }
        std::fprintf(out, "\n");
      }
      std::fclose(out);
    }
  }
  struct Group {
    RecentDraw draw;
    uint32_t draws = 0;
    uint64_t indices = 0;
  };
  std::vector<std::pair<uint64_t, Group>> groups;
  std::unordered_map<uint64_t, size_t> index_of;
  for (uint64_t i = begin; i < total; ++i) {
    const RecentDraw& d = recent_draws_[i % kRecentDraws];
    if (d.frame + 3 < now) continue;
    uint64_t key = d.frame;
    key = key * 1099511628211ull ^ d.vs;
    key = key * 1099511628211ull ^ d.ps;
    key = key * 1099511628211ull ^ d.pipeline;
    key = key * 1099511628211ull ^ d.framebuffer;
    auto [it, fresh] = index_of.try_emplace(key, groups.size());
    if (fresh) groups.push_back({key, Group{d, 0, 0}});
    Group& group = groups[it->second].second;
    ++group.draws;
    group.indices += d.count;
  }
  // Newest pairs first, then the rest in the order they were recorded.
  std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
    return a.second.draw.pair_first_frame > b.second.draw.pair_first_frame;
  });
  size_t shown = 0;
  for (const auto& [key, group] : groups) {
    if (shown++ >= 160) break;
    const RecentDraw& d = group.draw;
    REXLOG_ERROR(
        "Renut: Plume   frame {}{} vs={:016X} ps={:016X} x{} indices={} kind={} first={} pipeline={:X} fb={:X} "
        "s0={}B@{:X} s10={}B@{:X}",
        d.frame, d.pair_first_frame + 3 >= now ? " NEW" : "", d.vs, d.ps, group.draws, group.indices, d.kind,
        d.first, d.pipeline, d.framebuffer, d.s0_size, d.s0_addr, d.s10_size, d.s10_addr);
  }
}

void PlumeCommandProcessor::SyncEvery() {
  if (!list_open_) return;
  L()->end();
  list_open_ = false;
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  VkFence vk_fence = static_cast<VulkanCommandFence*>(fence_.get())->vk;
  const VkResult result = vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, 3000000000ull);
  if (result == VK_SUCCESS) vkResetFences(vk_device, 1, &vk_fence);
  StartList();
}

void PlumeCommandProcessor::SyncAfterDraw(const DrawPacket& pk) {
  if (!list_open_) return;
  L()->end();
  list_open_ = false;
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  VkFence vk_fence = static_cast<VulkanCommandFence*>(fence_.get())->vk;
  const auto wait_start = std::chrono::steady_clock::now();
  const VkResult result = vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, 3000000000ull);
  const auto wait_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count();
  ++sync_draw_count_;
  sync_recent_[sync_draw_count_ % sync_recent_.size()] = fmt::format("#{}: {}", sync_draw_count_, sync_draw_info_);
  if (result != VK_SUCCESS) {
    {
      unsigned long long avail_kb = 0;
      if (FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, meminfo)) {
          if (std::sscanf(line, "MemAvailable: %llu kB", &avail_kb) == 1) break;
        }
        std::fclose(meminfo);
      }
      REXLOG_ERROR("Renut: Plume system memory at the failure: {} MB available", avail_kb >> 10);
    }
    for (size_t i = 1; i <= sync_recent_.size(); ++i) {
      const std::string& line = sync_recent_[(sync_draw_count_ + i) % sync_recent_.size()];
      if (!line.empty()) REXLOG_ERROR("Renut: Plume   recent draw {}", line);
    }
    REXLOG_ERROR(
        "Renut: Plume GPU failed ({}) after {} ms on synced draw #{} in frame {}: vs={:016X} ps={:016X} kind={} "
        "count={} first={} base={} pipeline={}",
        int(result), wait_ms, sync_draw_count_, swaps_, pk.vs_hash, pk.ps_hash, int(pk.kind), pk.count, pk.first,
        pk.base_vertex, static_cast<const void*>(pk.pipeline));
    REXLOG_ERROR("Renut: Plume   {} streams of synced draws had a stale kept window before this one", sync_stale_streams_);
    const auto first_ok = sync_first_ok_.find(pk.vs_hash * 1099511628211ull ^ pk.ps_hash);
    if (first_ok != sync_first_ok_.end()) REXLOG_ERROR("Renut: Plume   first OK draw of this pair {}", first_ok->second);
    sync_draws_ = false;
  } else {
    vkResetFences(vk_device, 1, &vk_fence);
    if (sync_draw_info_.find("STALE") != std::string::npos && sync_stale_logged_ < 200) {
      ++sync_stale_logged_;
      REXLOG_INFO("Renut: Plume stale window under draw #{} frame {}: {}", sync_draw_count_, swaps_, sync_draw_info_);
    }
    if (sync_watch_ && (pk.vs_hash == sync_watch_ || pk.ps_hash == sync_watch_) && sync_watch_count_ < 600) {
      ++sync_watch_count_;
      REXLOG_INFO("Renut: Plume watched draw #{} frame {} pipeline={} fb={} ok: {}", sync_draw_count_, swaps_,
                  static_cast<const void*>(pk.pipeline), static_cast<const void*>(pk.framebuffer), sync_draw_info_);
    }
    // Each pair is named once, the first time it ran to completion.
    if (sync_seen_.insert(pk.vs_hash * 1099511628211ull ^ pk.ps_hash).second) {
      sync_first_ok_[pk.vs_hash * 1099511628211ull ^ pk.ps_hash] =
          fmt::format("frame {} pipeline={} fb={}: {}", swaps_, static_cast<const void*>(pk.pipeline),
                      static_cast<const void*>(pk.framebuffer), sync_draw_info_);
      REXLOG_INFO("Renut: Plume synced draw #{} ok, new pair (frame {}): vs={:016X} ps={:016X} count={}",
                  sync_draw_count_, swaps_, pk.vs_hash, pk.ps_hash, pk.count);
    }
  }
  StartList();
}

// Records the draws that are waiting, grouped by pipeline. The groups keep the
// order in which each pipeline first appeared and the draws keep their order
// inside a group, so a window whose draws share one pipeline is untouched.
void PlumeCommandProcessor::FlushDeferred(uint32_t reason) {
  if (deferred_.empty()) return;
  ++stats_.flush_windows[reason & 7];
  stats_.flush_draws[reason & 7] += deferred_.size();
  // Taken out first: recording goes through L(), which must find nothing waiting.
  flushing_.swap(deferred_);
  flushing_slots_.swap(deferred_slots_);
  deferred_.clear();
  deferred_slots_.clear();
  const size_t n = flushing_.size();
  defer_order_.resize(n);
  defer_groups_.clear();
  defer_group_start_.clear();
  uint32_t before = 0;
  for (size_t i = 0; i < n; ++i) {
    const DrawPacket& pk = flushing_[i];
    if (i == 0 || pk.pipeline != flushing_[i - 1].pipeline) ++before;
    auto [it, fresh] = defer_groups_.try_emplace(pk.pipeline, uint32_t(defer_group_start_.size()));
    if (fresh) defer_group_start_.push_back(0);
    ++defer_group_start_[it->second];
  }
  // Counts to start positions, then each draw to its place.
  uint32_t at = 0;
  for (uint32_t& entry : defer_group_start_) {
    const uint32_t count = entry;
    entry = at;
    at += count;
  }
  for (size_t i = 0; i < n; ++i) {
    defer_order_[defer_group_start_[defer_groups_[flushing_[i].pipeline]]++] = uint32_t(i);
  }
  uint32_t after = 0;
  for (size_t i = 0; i < n; ++i) {
    if (i == 0 || flushing_[defer_order_[i]].pipeline != flushing_[defer_order_[i - 1]].pipeline) ++after;
  }
  ++stats_.defer_windows;
  stats_.defer_binds_before += before;
  stats_.defer_binds_after += after;
  if (!HizFlushWindow(n)) {
    const bool async = RecActive();
    for (size_t i = 0; i < n; ++i) {
      const DrawPacket& pk = flushing_[defer_order_[i]];
      if (async) RecQueue(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
      else EmitDraw(pk, flushing_slots_.data() + pk.first_slot, pk.slot_count);
    }
  }
  flushing_.clear();
  flushing_slots_.clear();
}


// The SDK works a resolve out from the registers and from the three vertices of
// the rectangle the guest's copy draw would have drawn (fetch constant 0), which
// it applies the window offset and the scissor to. Here the rectangle is known
// (x0/y0 inclusive, x1/y1 exclusive, from the Direct3D call itself), so the SDK
// is handed a register file that says exactly that: the same registers, with
// fetch constant 0 pointing at three vertices of the rectangle, and no window
// offset or scissor to apply.
// ---------------------------------------------------------------------------
// Hi-Z: boxes, frustum and scissor culling (see hiz_model.h)
// ---------------------------------------------------------------------------

}  // namespace renut::gpu
