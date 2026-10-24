// Frame start, render targets, framebuffers and resolved images.
#include "internal.h"

namespace renut::gpu {

// Moves to the next frame in flight and opens its command list. Waiting here,
// rather than after the submission that ended the last frame, is what lets the
// GPU run a frame while this one is recorded.
// Nothing in these caches was ever freed. Every trip between levels added the
// new level's textures on top of the old ones until the 4096 2D descriptors ran
// out (about four trips): from then on every new texture sampled the white one
// in slot 0 and the picture collapsed to white. The 16 cube descriptors ran
// out long before that. Whatever has not been sampled for a while goes; it is
// decoded again if it comes back, and a resolve is only dropped when it has
// been neither written nor read.
void Targets::EvictIdleTextures() {
  const uint32_t free_2d = kMaxTextures2D - next_descriptor_ + uint32_t(free_descriptors_.size());
  const uint32_t free_3d = kMaxTextures3D - next_3d_descriptor_ + uint32_t(free_3d_descriptors_.size());
  const uint32_t free_cube = kMaxTexturesCube - next_cube_descriptor_ + uint32_t(free_cube_descriptors_.size());
  // About a minute at the game's frame rate, a few seconds when a set runs low.
  auto idle_for = [&](bool short_of) {
    return short_of ? uint64_t(60) : (backend.low_memory_ ? uint64_t(300) : uint64_t(900));
  };
  auto idle = [&](uint64_t frame, uint64_t limit) { return frame == ~0ull || backend.swaps_ - frame >= limit; };
  uint32_t evicted = 0;

  const uint64_t limit_2d = idle_for(free_2d < 512);
  for (auto it = guest_textures_.begin(); it != guest_textures_.end();) {
    if (!idle(it->second.checked_frame, limit_2d)) { ++it; continue; }
    RetireGuestTexture(it->second);
    it = guest_textures_.erase(it);
    ++evicted;
  }
  // A budget on top of the idle rule, for a host that cannot wait for a texture
  // to have gone unused: the oldest go first, never one drawn in the last few
  // frames (the frames in flight may still read it).
  if (texture_budget_) {
    uint64_t bytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> by_age;  // frame last checked, key
    for (const auto& [key, t] : guest_textures_) {
      if (!t.texture) continue;
      bytes += HostTextureBytes(t);
      by_age.emplace_back(t.checked_frame == ~0ull ? 0 : t.checked_frame, key);
    }
    if (bytes > texture_budget_) {
      std::sort(by_age.begin(), by_age.end());
      for (const auto& [frame, key] : by_age) {
        if (bytes <= texture_budget_ || backend.swaps_ - frame < 3) break;
        auto it = guest_textures_.find(key);
        if (it == guest_textures_.end()) continue;
        bytes -= HostTextureBytes(it->second);
        RetireGuestTexture(it->second);
        guest_textures_.erase(it);
        ++evicted;
      }
    }
  }
  // Once, not per erased entry: the memo is 64 KB.
  backend.textures.texture_memo_.Clear();
  ++object_epoch_;
  const uint64_t limit_3d = idle_for(free_3d < 16);
  for (auto* map : {&backend.textures.guest_stacks_, &backend.textures.guest_volumes_}) {
    for (auto it = map->begin(); it != map->end();) {
      if (!idle(it->second.checked_frame, limit_3d)) { ++it; continue; }
      if (it->second.texture) retired_slots_[backend.slot_].push_back(std::move(it->second.texture));
      if (it->second.descriptor) retired_3d_descriptor_slots_[backend.slot_].push_back(it->second.descriptor);
      it = map->erase(it);
      ++evicted;
    }
  }
  const uint64_t limit_cube = idle_for(free_cube < 4);
  for (auto it = guest_cubes_.begin(); it != guest_cubes_.end();) {
    if (!idle(it->second.checked_frame, limit_cube)) { ++it; continue; }
    if (it->second.view) retired_views_[backend.slot_].push_back(std::move(it->second.view));
    if (it->second.texture) retired_slots_[backend.slot_].push_back(std::move(it->second.texture));
    if (it->second.descriptor) retired_cube_descriptor_slots_[backend.slot_].push_back(it->second.descriptor);
    it = guest_cubes_.erase(it);
    ++evicted;
  }
  const uint64_t limit_resolved = free_2d < 512 ? 120 : 1800;
  for (auto it = resolved_.begin(); it != resolved_.end();) {
    Resolved& r = it->second;
    if (!idle(r.written_frame, limit_resolved) || !idle(r.checked_frame, limit_resolved)) { ++it; continue; }
    RetireSwizzled(r);
    if (r.alias) RetireFramebuffers();
    RetireTexture(r.texture, r.descriptor);
    RetireTexture(r.fitted, r.fitted_descriptor);
    it = resolved_.erase(it);
    InvalidateResolvedLookups();
    ++evicted;
  }
  bool kept_erased = false;
  for (auto it = backend.streams.kept_streams_.begin(); it != backend.streams.kept_streams_.end();) {
    if (backend.swaps_ - it->second.last_used < 600) { ++it; continue; }
    if (it->second.buffer) {
      backend.streams.retired_buffers_[backend.slot_].push_back(std::move(it->second.buffer));
      backend.streams.kept_bytes_ -= it->second.size;
    }
    it = (++backend.draw.stream_epoch_, backend.streams.kept_streams_.erase(it));
    kept_erased = true;
  }
  if (kept_erased) backend.streams.kept_memo_.Clear();
  if (evicted) {
    REXLOG_INFO("Renut: Plume evicted {} idle textures (free descriptors 2D {} 3D {} cube {})", evicted,
                free_2d, free_3d, free_cube);
  }
}

void Targets::BeginFrame() {
  backend.recorder.SyncRecorder();
  backend.slot_ = (backend.slot_ + 1) % kPlumeFramesInFlight;
  // The slot's command list, ring slice and retired resources are all still in
  // use until the frame that last held it has finished.
  const auto wait_start = std::chrono::steady_clock::now();
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForSlot(backend.slot_);
  backend.stats_.slot_wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - wait_start)
                                      .count());
  backend.profiling.ReadTimestamps(backend.slot_);
  backend.profiling.ReadPairTimestamps(backend.slot_);
  backend.census.BeginSlot(backend.slot_);
  backend.skip_mask_ = uint32_t(REXCVAR_GET(renut_plume_skip));
  backend.streams.ApplyWritebacks(backend.slot_);
  list_ = backend.lists_[backend.slot_].get();
  slice_base_ = slice_origin_ + uint64_t(backend.slot_) * backend.upload_slice_size_;
  for (uint32_t index : retired_descriptor_slots_[backend.slot_]) {
    backend.set_2d_->setTexture(index, backend.white_2d_.get(), RenderTextureLayout::SHADER_READ);
    written_2d_[index] = {backend.white_2d_.get(), nullptr};
    free_descriptors_.push_back(index);
  }
  retired_descriptor_slots_[backend.slot_].clear();
  for (uint32_t index : retired_3d_descriptor_slots_[backend.slot_]) {
    backend.set_3d_->setTexture(index, backend.white_3d_.get(), RenderTextureLayout::SHADER_READ);
    free_3d_descriptors_.push_back(index);
  }
  retired_3d_descriptor_slots_[backend.slot_].clear();
  for (uint32_t index : retired_cube_descriptor_slots_[backend.slot_]) {
    backend.set_cube_->setTexture(index, backend.white_cube_.get(), RenderTextureLayout::SHADER_READ, backend.white_cube_view_.get());
    free_cube_descriptors_.push_back(index);
  }
  retired_cube_descriptor_slots_[backend.slot_].clear();
  if (retire_delay_) {
    // Debug: hold what the frames in flight no longer need a while longer.
    if (retire_mask_ & 1) for (auto& object : retired_framebuffer_slots_[backend.slot_]) delayed_buffers_.emplace_back(backend.swaps_, std::shared_ptr<void>(std::move(object)));
    if (retire_mask_ & 2) for (auto& object : retired_slots_[backend.slot_]) delayed_buffers_.emplace_back(backend.swaps_, std::shared_ptr<void>(std::move(object)));
    if (retire_mask_ & 4) for (auto& object : retired_views_[backend.slot_]) delayed_buffers_.emplace_back(backend.swaps_, std::shared_ptr<void>(std::move(object)));
    if (retire_mask_ & 8) for (auto& object : backend.streams.retired_buffers_[backend.slot_]) delayed_buffers_.emplace_back(backend.swaps_, std::shared_ptr<void>(std::move(object)));
  }
  retired_framebuffer_slots_[backend.slot_].clear();
  retired_slots_[backend.slot_].clear();
  retired_views_[backend.slot_].clear();
  backend.streams.retired_buffers_[backend.slot_].clear();
  backend.patches.patch_out_used_ = 0;
  while (!delayed_buffers_.empty() && backend.swaps_ - delayed_buffers_.front().first >= retire_delay_) {
    delayed_buffers_.pop_front();
  }
  if (backend.swaps_ - last_eviction_ >= (backend.low_memory_ ? 8u : 30u)) {
    last_eviction_ = backend.swaps_;
    EvictIdleTextures();
  }
  StartList();
  // Only here, never from Flush: a mid-frame flush keeps recording into the
  // same range, and resetting it would drop what the first submission wrote.
  pass_marks_[backend.slot_].clear();
  backend.profiling.timestamps_used_ = 0;
  if (backend.timestamp_pool_ != VK_NULL_HANDLE) {
    vkCmdResetQueryPool(static_cast<VulkanCommandList*>(backend.recorder.L())->vk, backend.timestamp_pool_,
                        backend.slot_ * kMaxTimestamps, kMaxTimestamps);
    backend.profiling.WriteTimestamp();
  }
  backend.census.ResetSlot(backend.slot_);
  pair_capture_ = false;
  backend.profiling.pair_used_ = 0;
  backend.profiling.pair_marks_[backend.slot_].clear();
  if (backend.pair_pool_ != VK_NULL_HANDLE && backend.swap.pairprof_frames_left_ > 0) {
    vkCmdResetQueryPool(static_cast<VulkanCommandList*>(backend.recorder.L())->vk, backend.pair_pool_, backend.slot_ * kPairQueries,
                        kPairQueries);
    pair_capture_ = true;
    --backend.swap.pairprof_frames_left_;
  }
}

// Resets the per-frame state that lives in the ring slice and opens the list.
void Targets::StartList() {
  backend.recorder.SyncRecorder();
  backend.streams.upload_used_ = slice_base_;
  backend.streams.uploaded_streams_.clear();
  ++backend.draw.upload_generation_;
  for (auto& stage : backend.draw.constant_offsets_) {
    for (uint64_t& offset : stage) offset = UINT64_MAX;
  }
  backend.draw.shared_offset_ = UINT64_MAX;
  backend.draw.shared_cold_offset_ = UINT64_MAX;
  backend.draw.vs_all_offset_ = UINT64_MAX;
  // A new command list starts with no push constants of its own.
  pushed_constants_[0] = pushed_constants_[1] = pushed_constants_[2] = UINT64_MAX;
  std::fill(std::begin(backend.recorder.bound_constant_offsets_), std::end(backend.recorder.bound_constant_offsets_), UINT32_MAX);
  backend.recorder.viewport_bound_ = backend.recorder.scissor_bound_ = false;
  bound_vertex_buffers_.fill(VertexBinding{});
  bound_index_buffer_ = nullptr;
  bound_index_format_ = RenderFormat::UNKNOWN;
  backend.recorder.L()->begin();
  backend.recorder.list_open_ = true;
  bound_framebuffer_ = nullptr;
  backend.recorder.bound_pipeline_ = nullptr;
  state_bound_ = false;
  // Two frames share the render targets, and a target whose layout does not
  // change gets no barrier of its own, so nothing would otherwise stop this
  // frame's writes from reaching the GPU alongside the last one's. One barrier
  // at the top orders this submission after everything already queued. It costs
  // GPU/GPU overlap, which is not what the pipelining is for - the win is this
  // list being recorded while the previous one runs.
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(static_cast<VulkanCommandList*>(backend.recorder.L())->vk,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                       &barrier, 0, nullptr, 0, nullptr);
}

// Runs what was recorded so far and waits, to make room in the upload ring.
// A full drain: the slice cannot be rewritten while any frame still reads it.
void Targets::Flush() {
  backend.recorder.FlushDeferred();
  if (!backend.recorder.list_open_) return;
  backend.recorder.L()->end();
  backend.recorder.list_open_ = false;
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForAllSlots();
  const RenderCommandList* lists = list_;
  backend.queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, backend.recorder.fence_.get());
  backend.queue_->waitForCommandFence(backend.recorder.fence_.get());
  StartList();
}

void Targets::Transition(RenderTexture* texture, RenderTextureLayout& current,
                                       RenderTextureLayout layout) {
  if (current == layout) return;
  RenderBarrierStages stages = (layout == RenderTextureLayout::COPY_SOURCE ||
                                layout == RenderTextureLayout::COPY_DEST)
                                   ? RenderBarrierStage::COPY
                                   : RenderBarrierStage::GRAPHICS;
  backend.recorder.L()->barriers(stages, RenderTextureBarrier(texture, layout));
  ++backend.stats_.barriers;
  current = layout;
  // A barrier ends the render pass, so every layout change made while one is
  // bound splits it in two. Counting them by destination says which is worth
  // chasing: the frame runs 82 passes, and one carrying a single draw costs
  // about 0.3 ms of GPU whatever it draws.
  if (bound_framebuffer_) {
    ++backend.stats_.split_by[uint32_t(layout) & 7];
    // On a tiler the pass's fragment work runs at vkCmdEndRenderPass, which
    // this barrier just triggered - so the mark belongs here, after it, for the
    // interval since the pass opened to be that pass's own cost.
    PassMark mark;
    mark.resolve = 1;
    pass_marks_[backend.slot_].push_back(mark);
    backend.profiling.WriteTimestamp();
  }
  bound_framebuffer_ = nullptr;  // barriers end the render pass
}



uint32_t Targets::SwizzledResolved(Resolved& r, bool fitted, uint32_t swizzle, bool stencil_first) {
  RenderTexture* texture = fitted ? r.fitted.get() : r.texture.get();
  if (!texture) return 0;
  // Only a resolve that kept its stencil (RG32F) reads differently as 8_8_8_8.
  stencil_first = stencil_first && r.format == RenderFormat::R32G32_FLOAT;
  Resolved::SwizzledView& s = r.swizzled[swizzle | (uint32_t(fitted) << 12) | (uint32_t(stencil_first) << 13)];
  if (s.texture != texture || !s.view || !s.descriptor) {
    if (s.view) retired_views_[backend.slot_].push_back(std::move(s.view));
    if (s.descriptor) retired_descriptor_slots_[backend.slot_].push_back(s.descriptor);
    // Xenos swizzle lanes: 0..3 pick a channel, every one of them the
    // depth; 4 is 0, 5 is 1. Fetched as 8_8_8_8, a k_24_8 surface has its
    // stencil in the first channel (the low byte), kept in G.
    auto lane = [stencil_first](uint32_t code) {
      if (code == 0 && stencil_first) return RenderSwizzle::G;
      return code < 4 ? RenderSwizzle::R : code == 5 ? RenderSwizzle::ONE : RenderSwizzle::ZERO;
    };
    // An aliased depth map is read through its own format (the view takes the
    // depth aspect); its fitted copy is the R32_FLOAT a depth resolve makes.
    RenderTextureViewDesc desc =
        RenderTextureViewDesc::Texture2D(fitted && r.alias ? RenderFormat::R32_FLOAT : r.format);
    desc.componentMapping = RenderComponentMapping(lane(swizzle & 7), lane((swizzle >> 3) & 7),
                                                   lane((swizzle >> 6) & 7), lane((swizzle >> 9) & 7));
    s.view = texture->createTextureView(desc);
    s.descriptor = AllocateTextureDescriptor();
    s.texture = texture;
    if (!s.view || !s.descriptor) return 0;
    SetTexture2D(s.descriptor, texture, s.view.get());
  }
  return s.descriptor;
}

uint32_t Targets::FittedResolved(Resolved& r, uint32_t width, uint32_t height) {
  if (!r.fitted || r.fitted_width != width || r.fitted_height != height) {
    RetireSwizzled(r);
    RetireTexture(r.fitted, r.fitted_descriptor);
    r.fitted = backend.device_->createTexture(
        RenderTextureDesc::Texture2D(width, height, 1, r.alias ? RenderFormat::R32_FLOAT : r.format));
    r.fitted_width = width;
    r.fitted_height = height;
    r.fitted_layout = RenderTextureLayout::UNKNOWN;
    r.fitted_generation = UINT64_MAX;
    r.fitted_descriptor = AllocateTextureDescriptor();
    if (!r.fitted || !r.fitted_descriptor) return 0;
    SetTexture2D(r.fitted_descriptor, r.fitted.get());
  }
  if (r.fitted_generation != r.generation) {
    if (r.alias) {
      // The depth image goes through CopyDepth, which takes it and its layout
      // from a target.
      Target image;
      image.texture = std::move(r.texture);
      image.layout = r.layout;
      image.depth = true;
      image.width = r.width;
      image.height = r.height;
      Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::COPY_DEST);
      backend.resolve.CopyDepth(image, 0, 0, width, height, r.fitted.get(), 0, 0, false);
      r.texture = std::move(image.texture);
      r.layout = image.layout;
    } else {
      Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
      Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::COPY_DEST);
      RenderBox box(0, 0, int32_t(width), int32_t(height), 0, 1);
      backend.recorder.L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.fitted.get()),
                               RenderTextureCopyLocation::Subresource(r.texture.get()), 0, 0, 0, &box);
    }
    r.fitted_generation = r.generation;
  }
  Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::SHADER_READ);
  return r.fitted_descriptor;
}



// ---------------------------------------------------------------------------
// Render targets per Direct3D surface (native lists). The surface object and
// its header name the target, so two surfaces sharing EDRAM are two targets,
// and its size is the surface's own (D3DSurface_GetDesc: +36 holds width - 1
// from bit 18 and height - 1 from bit 3; the width is the pitch, as for the
// EDRAM targets).

Target* Targets::NativeTarget(uint32_t slot, uint32_t info_register) {
  const ListSurface& s = native_surfaces_[slot];
  if (!s.object) {
    ++surface_missing_;
    return nullptr;
  }
  if (s.info != info_register) ++surface_mismatch_;
  return SurfaceTarget(slot);
}

Target* Targets::SurfaceTarget(uint32_t slot) {
  const ListSurface& s = native_surfaces_[slot];
  if (!s.object) return nullptr;
  const bool depth = slot == 4;
  const uint32_t format = depth ? (s.info >> 16) & 1 : (s.info >> 16) & 0xF;
  const uint32_t header = s.surface_info * 0x9E3779B1u ^ s.info * 0x85EBCA77u ^ s.dims * 0xC2B2AE3Du ^
                          uint32_t(depth);
  const uint64_t key = kSurfaceTargetKey | (uint64_t(header & 0x7FFFFFFFu) << 32) | s.object;
  Target& target = targets_[key];
  target.last_used = ++target_serial_;
  target.used_frame = backend.swaps_;
  if (target.texture) return &target;
  const uint32_t pitch = s.surface_info & 0x3FFF;
  uint32_t width = std::max({pitch, ((s.dims >> 18) & 0x3FFF) + 1, 16u});
  uint32_t height = std::max(((((s.dims >> 3) & 0x7FFF) + 1) + 15) & ~15u, 16u);
  // A D24S8 shadow map is its D3D surface's size, not the EDRAM pitch padded to
  // whole tiles (1040 for 1024): the extra columns are never drawn, and sampled
  // as a resolved texture in place (AliasDepthResolve) they would stretch the map.
  if (depth && format == 0) {
    const uint32_t exact = std::max(((s.dims >> 18) & 0x3FFF) + 1, 16u);
    if (exact < width) width = exact;
  }
  // The dynamic shadow maps are the only D24S8 depth surfaces (the scene's is
  // D24FS8) of this size: drawn at the shadow quality's fraction of it.
  uint32_t scale = 4;
  if (depth && format == 0 && shadow_scale_ != 4 && std::min(width, height) >= 640) {
    scale = shadow_scale_;
  } else if (internal_scale_ != 4 && width == 1280 && height >= 704 && height <= 736) {
    // The scene's colour and depth: the internal resolution option.
    scale = internal_scale_;
  }
  if (scale != 4) {
    width = std::max((width * scale + 3) / 4, 16u);
    height = std::max((height * scale + 3) / 4, 16u);
  }
  const RenderFormat rf = depth ? RenderFormat::D32_FLOAT_S8_UINT
                                : ColorFormat(xenos::ColorRenderTargetFormat(format));
  target.texture = backend.device_->createTexture(depth ? RenderTextureDesc::DepthTarget(width, height, rf)
                                                : RenderTextureDesc::ColorTarget(width, height, rf));
  if (!target.texture) {
    targets_.erase(key);
    return nullptr;
  }
  target.format = rf;
  target.width = width;
  target.height = height;
  target.depth = depth;
  target.scale = scale;
  target.layout = RenderTextureLayout::UNKNOWN;
  target.surface = s.object;
  target.surface_info = s.info;
  ++surface_created_;
  RetireFramebuffers();
  return &target;
}

void Targets::RetireFramebuffers() {
  backend.recorder.SyncRecorder();
  for (auto& [key, framebuffer] : framebuffers_)
    retired_framebuffer_slots_[backend.slot_].push_back(std::move(framebuffer));
  framebuffers_.clear();
  ++object_epoch_;
  bound_framebuffer_ = nullptr;
}

bool Targets::GrowSurfaceTarget(Target& target, uint32_t width, uint32_t height) {
  width = std::max(width, target.width);
  height = std::max(height, target.height);
  if (width == target.width && height == target.height) return true;
  auto texture = backend.device_->createTexture(target.depth ? RenderTextureDesc::DepthTarget(width, height, target.format)
                                                     : RenderTextureDesc::ColorTarget(width, height, target.format));
  if (!texture) return false;
  if (target.texture) retired_slots_[backend.slot_].push_back(std::move(target.texture));
  target.texture = std::move(texture);
  target.width = width;
  target.height = height;
  target.layout = RenderTextureLayout::UNKNOWN;
  target.restore_key = 0;  // what it held was lost with its image
  target.restore_image = nullptr;
  target.alias_cycles = 0;
  target.aliased_since_clear = false;
  ++surface_grown_;
  RetireFramebuffers();
  return true;
}

void Targets::MatchSurfaceSizes(std::array<Target*, 4>& colors, uint32_t color_count, Target*& depth) {
  Target* first = color_count ? colors[0] : depth;
  if (!first) return;
  uint32_t width = first->width, height = first->height;
  for (uint32_t i = 1; i < color_count; ++i) {
    if (colors[i]) width = std::max(width, colors[i]->width), height = std::max(height, colors[i]->height);
  }
  if (depth) width = std::max(width, depth->width), height = std::max(height, depth->height);
  for (uint32_t i = 0; i < color_count; ++i) {
    if (colors[i] && !GrowSurfaceTarget(*colors[i], width, height)) colors[i] = nullptr;
  }
  if (depth && !GrowSurfaceTarget(*depth, width, height)) depth = nullptr;
}

// Targets of surfaces not drawn to for ten seconds go (a released surface's
// object is reused for another).
void Targets::EvictSurfaceTargets() {
  if (backend.swaps_ % 60) return;
  bool evicted = false;
  for (auto it = targets_.begin(); it != targets_.end();) {
    if ((it->first & kSurfaceTargetKey) && it->second.used_frame + 600 < backend.swaps_) {
      if (it->second.texture) retired_slots_[backend.slot_].push_back(std::move(it->second.texture));
      it = targets_.erase(it);
      ++surface_evicted_;
      evicted = true;
    } else {
      ++it;
    }
  }
  if (evicted) RetireFramebuffers();
}

std::string Targets::SurfaceReport() {
  size_t count = 0;
  for (const auto& [key, t] : targets_) count += (key & kSurfaceTargetKey) ? 1 : 0;
  std::string out = fmt::format(
      "== surfaces: {} targets now, {} created, {} grown, {} evicted; register/surface mismatches {}, "
      "slots without a surface {}\n== resolves: {} into their texture object, {} by address\n"
      "== executor: fences {}, presents {}, callbacks {}, "
      "vblank waits {} ({} ms, {} timed out); streams kept by buffer {}, by window {}\n",
      count, surface_created_, surface_grown_, surface_evicted_, surface_mismatch_, surface_missing_,
      object_resolves_, address_resolves_count_,
      backend.lists.native_fences_, backend.lists.native_presents_, backend.lists.native_callbacks_, native_waits_, native_wait_us_ / 1000,
      native_wait_timeouts_, object_streams_, window_streams_);
  for (const auto& [key, t] : targets_) {
    if (!(key & kSurfaceTargetKey)) continue;
    out += fmt::format("   surface {:08X} {} {}x{} base {} format {} last frame {}\n", t.surface,
                       t.depth ? "depth" : "color", t.width, t.height, t.surface_info & 0xFFF,
                       t.depth ? (t.surface_info >> 16) & 1 : (t.surface_info >> 16) & 0xF, t.used_frame);
  }
  return out;
}

RenderFramebuffer* Targets::GetFramebuffer(const std::array<Target*, 4>& colors,
                                                         uint32_t color_count, Target* depth) {
  // What SetFramebuffer labels the next pass with.
  color_count_of_pass_ = color_count;
  depth_of_pass_ = depth != nullptr;
  // The same attachments as the draw before: the same framebuffer, without
  // hashing them or asking the map.
  const RenderTexture* images[5] = {};
  for (uint32_t i = 0; i < color_count; ++i) images[i] = colors[i]->texture.get();
  images[4] = depth ? depth->texture.get() : nullptr;
  if (backend.textures.objkeys_ && last_framebuffer_.framebuffer && last_framebuffer_.epoch == object_epoch_ &&
      last_framebuffer_.color_count == color_count &&
      std::memcmp(last_framebuffer_.images, images, sizeof(images)) == 0) {
    return last_framebuffer_.framebuffer;
  }
  uint64_t key = 0;
  for (uint32_t i = 0; i < color_count; ++i) HashCombine(key, colors[i]->texture.get());
  HashCombine(key, depth ? depth->texture.get() : nullptr);
  HashCombine(key, color_count);
  auto& framebuffer = framebuffers_[key];
  if (!framebuffer) {
    const RenderTexture* attachments[4] = {};
    for (uint32_t i = 0; i < color_count; ++i) attachments[i] = colors[i]->texture.get();
    framebuffer = backend.device_->createFramebuffer(
        RenderFramebufferDesc(attachments, color_count, depth ? depth->texture.get() : nullptr));
  }
  std::memcpy(last_framebuffer_.images, images, sizeof(images));
  last_framebuffer_.color_count = color_count;
  last_framebuffer_.epoch = object_epoch_;
  last_framebuffer_.framebuffer = framebuffer.get();
  return framebuffer.get();
}

void Targets::SetFramebuffer(RenderFramebuffer* framebuffer) {
  const bool recorder = backend.recorder.OnRecorder();
  if (!recorder) backend.recorder.SyncRecorder();
  if (!state_bound_) {
    backend.recorder.L()->setGraphicsPipelineLayout(backend.layout_.get());
    backend.recorder.L()->setGraphicsDescriptorSet(backend.set_2d_.get(), 0);
    backend.recorder.L()->setGraphicsDescriptorSet(backend.set_3d_.get(), 1);
    backend.recorder.L()->setGraphicsDescriptorSet(backend.set_cube_.get(), 2);
    backend.recorder.L()->setGraphicsDescriptorSet(backend.set_samplers_.get(), 3);
    state_bound_ = true;
  }
  if (bound_framebuffer_ != framebuffer) {
    // setFramebuffer ends the pass that was open, and the next one starts at
    // the first draw, so the mark lands between the two.
    backend.recorder.L()->setFramebuffer(framebuffer);
    bound_framebuffer_ = framebuffer;
    ++backend.report.rec_stats_.passes;
    PassMark mark;
    mark.width = framebuffer->getWidth();
    mark.height = framebuffer->getHeight();
    mark.colors = recorder ? emit_colors_ : color_count_of_pass_;
    mark.depth = recorder ? emit_depth_ : depth_of_pass_;
    pass_marks_[backend.slot_].push_back(mark);
    backend.profiling.WriteTimestamp();
  }
}

Resolved& Targets::GetResolved(uint32_t address, RenderFormat format, uint32_t width,
                                             uint32_t height) {
  InvalidateResolvedLookups();
  Resolved& resolved = resolved_[ResolvedKey(address, format)];
  if (resolved.texture && resolved.format == format && resolved.width >= width &&
      resolved.height >= height) {
    return resolved;
  }
  uint32_t w = std::max(width, resolved.format == format ? resolved.width : 0u);
  uint32_t h = std::max(height, resolved.format == format ? resolved.height : 0u);
  RetireSwizzled(resolved);
  RetireTexture(resolved.texture, resolved.descriptor);
  resolved.texture = backend.device_->createTexture(RenderTextureDesc::Texture2D(w, h, 1, format));
  resolved.format = format;
  resolved.width = w;
  resolved.height = h;
  resolved.layout = RenderTextureLayout::UNKNOWN;
  resolved.descriptor = AllocateTextureDescriptor();
  return resolved;
}

// A shadow map's resolve without its copy. CopyDepth stages the depth through a
// buffer into R32_FLOAT, which is the same bits a depth image gives a fetch when
// it is sampled through a depth view, so the surface's image can simply become
// the resolved texture. The surface takes the image the previous map of this
// destination was in (the two swap every frame), or a new one the first time.
//
// The surface's contents are not there for the game any more. That is only
// seen if the game uses the surface again without clearing all of it, and then
// RestoreAliasedDepth copies them back from the resolved image, which is still
// the map as it was resolved - what the old path's surface held all along.
Resolved* Targets::AliasDepthResolve(Target& source, uint32_t address) {
  // The surface has a copy of its own pending: give it back before the image
  // goes, or the resolve would take an undefined one.
  if (source.restore_key) RestoreAliasedDepth(source, false);
  const RenderFormat format = source.format;
  const uint64_t key = ResolvedKey(address, format);
  Resolved& r = resolved_[key];
  // The name the texture is fetched by comes first, so that nothing has moved
  // when there is none to be had.
  if (!r.descriptor) r.descriptor = AllocateTextureDescriptor();
  if (!r.descriptor) {
    if (!r.texture) resolved_.erase(key);
    return nullptr;
  }
  // Another surface's map is in this destination: that surface gets its
  // contents back before its image is reused.
  if (r.alias && r.texture) {
    for (auto& [target_key, other] : targets_) {
      if (&other == &source || other.restore_key != key || other.restore_image != r.texture.get()) continue;
      if (other.alias_cycles >= 2) {
        // Its map is resolved in place and it is cleared whole before it is
        // used again, twice over: nothing of what it held is read.
        other.restore_key = 0;
        other.restore_image = nullptr;
        other.depth_undefined = true;
        ++backend.stats_.depth_abandoned;
      } else {
        RestoreAliasedDepth(other, false);
      }
    }
  }
  std::unique_ptr<RenderTexture> fresh;
  RenderTextureLayout fresh_layout = RenderTextureLayout::UNKNOWN;
  if (r.alias && r.texture && r.width == source.width && r.height == source.height) {
    fresh = std::move(r.texture);
    fresh_layout = r.layout;
  } else {
    fresh = backend.device_->createTexture(RenderTextureDesc::DepthTarget(source.width, source.height, format));
    if (!fresh) {
      if (!r.texture) {
        if (r.descriptor) retired_descriptor_slots_[backend.slot_].push_back(r.descriptor);
        resolved_.erase(key);
      }
      return nullptr;
    }
  }
  InvalidateResolvedLookups();
  RetireSwizzled(r);
  if (r.texture) {
    // An alias of another size: its descriptor stays, the image goes.
    RetireFramebuffers();
    retired_slots_[backend.slot_].push_back(std::move(r.texture));
  }
  r.texture = std::move(source.texture);
  r.layout = source.layout;
  r.format = format;
  r.width = source.width;
  r.height = source.height;
  r.alias = true;
  source.texture = std::move(fresh);
  source.layout = fresh_layout;
  source.restore_key = key;
  source.restore_image = r.texture.get();
  source.aliased_since_clear = true;
  ++backend.stats_.depth_aliased;
  return &r;
}

// What a depth surface that was resolved in place is used for next, for the
// pattern AliasDepthResolve waits for: a clear of all of it ends the cycle and
// counts, anything else breaks the run. `whole` is a clear that covered the
// surface; the caller restores or drops the contents itself.
void Targets::NoteAliasedUse(Target& target, bool whole_clear) {
  if (!target.aliased_since_clear) return;
  target.aliased_since_clear = false;
  target.alias_cycles = whole_clear ? target.alias_cycles + 1 : 0;
}

bool Targets::RestoreAliasedDepth(Target& target, bool full_clear) {
  const uint64_t key = target.restore_key;
  const RenderTexture* image = target.restore_image;
  target.restore_key = 0;
  target.restore_image = nullptr;
  if (!key) return false;
  if (full_clear) {
    ++backend.stats_.depth_dropped;
    return true;
  }
  auto found = resolved_.find(key);
  if (found == resolved_.end() || !found->second.alias || found->second.texture.get() != image ||
      !target.texture || found->second.width != target.width || found->second.height != target.height) {
    // The map was evicted or resolved over by another surface: what the target
    // held is gone, as it is for a target that was grown.
    return false;
  }
  Resolved& r = found->second;
  Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
  Transition(target.texture.get(), target.layout, RenderTextureLayout::COPY_DEST);
  backend.recorder.L()->copyTexture(target.texture.get(), r.texture.get());
  ++backend.stats_.depth_restored;
  return false;
}

}  // namespace renut::gpu
