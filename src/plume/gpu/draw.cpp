// IssueDraw: one guest draw, from register state to a recorded draw.
#include "internal.h"

namespace renut::gpu {

// plume-skip bits that only change a draw's own state (no depth test, no culling,
// no alpha to mask, no timestamps): they keep Hi-Z and the draw windows, so the
// zero-sample census can be compared with them on and off.
constexpr uint32_t kStateOnlySkipBits = 1024 | 131072 | 4096 | 67108864;

bool Draw::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info, bool) {
  struct InDraw {
    bool& flag;
    explicit InDraw(bool& f) : flag(f) { flag = true; }
    ~InDraw() { flag = false; }
  } in_draw(in_issue_draw_);
  if (g_predict.queued.load(std::memory_order_relaxed)) backend.pipelines.PredictPipelines();
  const RegisterFile& regs = *backend.register_file_;
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) return true;  // resolves are records
  ++backend.stats_.draws;
  const uint32_t draw_index = backend.swap.frame_draw_++;
  if (draw_index >= backend.swap.drawskip_first_ && draw_index < backend.swap.drawskip_end_) return true;
  for (const ConstOverride& o : backend.swap.const_overrides_) {
    if (o.bool_word) {
      uint32_t& word = backend.register_file_->values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + o.reg];
      word = (word | o.set) & ~o.clear;
      continue;
    }
    const uint32_t at = (o.pixel ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X) + o.reg * 4;
    std::memcpy(&backend.register_file_->values[at], o.value, sizeof(o.value));
    MarkConstantsWritten(at, 4);
  }

  auto* vs = static_cast<HostShader*>(backend.active_vertex_shader());
  if (trace_ && (!vs || !vs->entry)) {
    const auto dc = draw_util::GetNormalizedDepthControl(regs);
    Trace("dropped vs={:016X} prim={} n={} mode={} rt0={:X} ds={:X} z={}{}{} f{} colormask={:X}",
          vs ? vs->ucode_data_hash() : 0, uint32_t(prim_type), index_count, uint32_t(edram_mode),
          uint32_t(regs.Get<reg::RB_COLOR_INFO>().color_base),
          uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base), dc.z_enable ? "t" : "",
          dc.z_write_enable ? "w" : "", dc.stencil_enable ? "s" : "", uint32_t(dc.zfunc),
          regs[XE_GPU_REG_RB_COLOR_MASK]);
  }
  if (!vs || !vs->entry) {
    ++backend.stats_.no_vs;
    if (vs) {
      auto* ps_miss = static_cast<HostShader*>(backend.active_pixel_shader());
      const uint64_t ps_hash = ps_miss && ps_miss->entry ? ps_miss->entry->hash : 0;
      ++backend.report.missed_draws_[vs->ucode_data_hash()];
      if (missed_pairs_.insert(vs->ucode_data_hash() ^ ps_hash).second && missed_pairs_.size() <= 48) {
        REXLOG_INFO("Renut: Plume missing VS {:016X} with PS {:016X}, mode {}, prim {}, {} indices",
                    vs->ucode_data_hash(), ps_hash, uint32_t(edram_mode), uint32_t(prim_type), index_count);
      }
    }
    return true;
  }
  auto* ps = static_cast<HostShader*>(backend.active_pixel_shader());
  const bool color_pass = edram_mode == xenos::EdramMode::kColorDepth;
  if (color_pass && (!ps || !ps->entry)) {
    ++backend.stats_.no_ps;
    return true;
  }
  if (!color_pass) ps = nullptr;
  if (!vs->compiled) vs->compiled = backend.pipelines.store_->Build(backend.device_, vs->entry, true, true);
  if (ps && !ps->compiled) ps->compiled = backend.pipelines.store_->Build(backend.device_, ps->entry, false);
  if (!vs->compiled || (ps && !ps->compiled)) {
    ++backend.stats_.bad_shader;
    return true;
  }
  if (!vs->elements_decoded) DecodeLiveElements(*vs);

  // Primitive.
  RenderPrimitiveTopology topology = RenderPrimitiveTopology::UNKNOWN;
  bool point_list = false, rect_list = false, quad_list = false, patch_list = false;
  bool quad_patch = false;
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList:
      // Particles. Every point becomes a sprite in point_list.geom, sized by
      // what the recompiled vertex shader exports at the point-sprite location.
      topology = RenderPrimitiveTopology::POINT_LIST;
      point_list = true;
      break;
    case xenos::PrimitiveType::kLineList: topology = RenderPrimitiveTopology::LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = RenderPrimitiveTopology::LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleFan: topology = RenderPrimitiveTopology::TRIANGLE_FAN; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case xenos::PrimitiveType::kRectangleList:
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      rect_list = true;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      quad_list = true;
      break;
    case xenos::PrimitiveType::kQuadPatch:
      // The sea of the first world, as a grid of quads.
      quad_patch = true;
      [[fallthrough]];
    case xenos::PrimitiveType::kTrianglePatch:
      // The water. The guest's shader runs as the domain stage, once per point
      // of a patch that Plume expands itself: the driver here reports no
      // tessellation stage, and the expansion is the same triangles it would
      // have made.
      if (!vs->domain_decoded) {
        vs->domain_decoded = true;
        vs->domain_entry = backend.pipelines.store_->FindDomain(vs->ucode_key, vs->loose_key);
        if (vs->domain_entry) {
          vs->domain_compiled = backend.pipelines.store_->Build(backend.device_, vs->domain_entry, false, true);
          std::vector<LiveElement> unused;
          DecodeLiveElements(*vs, vs->domain_entry, unused, vs->domain_live_fetches);
        }
      }
      if (!vs->domain_compiled) break;
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      patch_list = true;
      break;
    default:
      break;
  }
  if (topology == RenderPrimitiveTopology::UNKNOWN) {
    ++backend.stats_.skipped_prim;
    if ((prim_type == xenos::PrimitiveType::kTrianglePatch || prim_type == xenos::PrimitiveType::kQuadPatch) &&
        patch_drop_logged_.insert(vs->entry->hash).second) {
      // Water with no domain form simply does not appear. tools/prepare.py dumps reads this line back.
      REXLOG_INFO(
          "Renut: Plume patch draw dropped: vs={:016X} has no domain form (list it in "
          "tools/shaders/domain-shaders.txt)",
          vs->entry->hash);
    }
    // The patches Plume could not tessellate go through here: the shader and
    // the tessellation mode say which.
    Trace("dropped prim={} n={} vs={:016X} ps={:016X} tess={} indexed={}", uint32_t(prim_type),
          index_count, vs->entry->hash, ps ? ps->entry->hash : 0,
          uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode),
          uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().source_select));
    return true;
  }
  if ((rect_list && !rect_list_gs_) || (point_list && !point_list_gs_)) {
    ++backend.stats_.skipped_prim;
    return true;
  }

  // Hi-Z: a draw whose box lies wholly outside the frustum or the scissor puts
  // nothing on the target; it is dropped before its targets, textures, streams,
  // constants and pipeline cost anything. Exact (hiz_model.h). The resolves a
  // dropped draw would have fetched are fetched by the draws that remain: only
  // mesh draws (triangles through a vertex model) are ever dropped, never the
  // full-screen passes that read the scene's copies.
  if (backend.report.hiz_mode_ && !trace_ && !(backend.skip_mask_ & ~kStateOnlySkipBits) && !backend.recorder.bind_skip_ && !sync_draws_ && !patch_list && !rect_list &&
      !point_list && vs->compiled && (!debug_shader_ || (vs->entry->hash != debug_shader_ &&
                                                          (!ps || ps->entry->hash != debug_shader_)))) {
    // One call in 16 is timed (the clock reads are a cost of their own).
    const bool timed = (++hiz_timer_phase_ & 15) == 0;
    backend.hiz.hiz_stage_timing_ = timed;
    std::chrono::steady_clock::time_point cull_start;
    if (timed) cull_start = std::chrono::steady_clock::now();
    const bool dropped = backend.hiz.HizCullDraw(*vs, prim_type, index_count, index_buffer_info, color_pass);
    if (timed) {
      backend.stats_.hiz_cull_ns += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - cull_start).count());
    }
    if (dropped) {
      ++backend.stats_.hiz_culled_total;
      return true;
    }
  }
  // Render targets.
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t pitch = surface.surface_pitch;
  if (!pitch) return backend.shaders.Drop("no pitch", vs, ps);
  const uint32_t height = std::clamp<uint32_t>(regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y, 16, 4096);
  const uint32_t color_mask = regs.Get<reg::RB_COLOR_MASK>().value;
  static constexpr Register kColorInfo[4] = {
      XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO,
      XE_GPU_REG_RB_COLOR3_INFO};
  static constexpr Register kBlendControl[4] = {
      XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2,
      XE_GPU_REG_RB_BLENDCONTROL3};
  std::array<Target*, 4> colors{};
  uint32_t color_count = 0;
  if (color_pass) {
    for (uint32_t i = 0; i < 4; ++i) {
      if ((color_mask >> (4 * i)) & 0xF) color_count = i + 1;
    }
    for (uint32_t i = 0; i < color_count; ++i) {
      reg::RB_COLOR_INFO info;
      info.value = regs[kColorInfo[i]];
      colors[i] = backend.targets.NativeTarget(i, info.value);
      if (!colors[i]) {
        ++backend.stats_.no_target;
        return true;
      }
    }
  }
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  Target* depth = nullptr;
  if (depth_control.z_enable || depth_control.stencil_enable) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    depth = backend.targets.NativeTarget(4, depth_info.value);
  }
  {
    backend.targets.MatchSurfaceSizes(colors, color_count, depth);
    for (uint32_t i = 0; i < color_count; ++i) {
      if (!colors[i]) {
        ++backend.stats_.no_target;
        return true;
      }
    }
  }
  if (!color_count && !depth) return backend.shaders.Drop("no color or depth", vs, ps);
  // Debug (plume-bind 16): drop depth-only draws - the shadow casters - to
  // price them.
  if ((backend.recorder.bind_skip_ & 16) && !color_count) return true;
  // files/plume-shadowmin: a shadow caster with fewer indices than this casts
  // no shadow. Only depth-only draws that write depth into a target that is not
  // the screen's: a stencil mark, a memory export or the scene's own depth is
  // never dropped. Off (0) by default.
  if (backend.swap.shadow_min_indices_ && !color_count && index_count < backend.swap.shadow_min_indices_ && pitch != 1280 &&
      depth_control.z_write_enable && !depth_control.stencil_enable &&
      !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) {
    ++backend.stats_.shadow_culled;
    return true;
  }
  // MatchSurfaceSizes may have recreated a texture that an earlier slot also uses.
  for (uint32_t i = 0; i < color_count; ++i) {
    backend.targets.Transition(colors[i]->texture.get(), colors[i]->layout, RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    // A surface whose image went to a depth resolve and is drawn to without
    // being cleared: what it held comes back (AliasDepthResolve).
    if (depth->restore_key) backend.targets.RestoreAliasedDepth(*depth, false);
    // Used again without a clear: not the pattern (and whatever it holds is what
    // it holds).
    if (depth->aliased_since_clear) backend.targets.NoteAliasedUse(*depth, false);
    depth->depth_undefined = false;
    backend.targets.Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
  }

  // Textures used by the draw: resolved render targets, by guest address.
  uint32_t texture_descriptor[32] = {};
  uint32_t cube_descriptor[32] = {};
  uint32_t stack_descriptor[32] = {};
  uint32_t sampler_descriptor[32] = {};
  // Only the slots the bound shaders actually sample. The game leaves the other
  // fetch constants set from earlier draws, and asking all 32 for a texture, a
  // sampler and a resolved target was most of what this path did.
  auto slots_of = [&](HostShader* shader) {
    if (!shader) return 0u;
    if (!shader->texture_mask_decoded) {
      shader->texture_mask = TextureMask(*shader);
      shader->texture_mask_decoded = true;
    }
    return shader->texture_mask;
  };
  uint32_t used_slots = slots_of(vs) | slots_of(ps);
  // Debug: go back to binding every slot, to compare against a capture.
  if (backend.skip_mask_ & 1048576) used_slots = ~0u;
  for (uint32_t remaining = used_slots; remaining;) {
    const uint32_t slot = uint32_t(__builtin_ctz(remaining));
    remaining &= remaining - 1;
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
    if (fetch.type != xenos::FetchConstantType::kTexture) continue;
    sampler_descriptor[slot] = backend.textures.GetSampler(fetch);
    Resolved* found = FindResolved(fetch.base_address << 12);
    if (found && found->descriptor) {
      Resolved& r = *found;
      if (backend.swap.depth_dump_ps_ && !backend.swap.depth_dump_ && r.depth && !r.alias && r.texture && ps &&
          ps->entry->hash == backend.swap.depth_dump_ps_) {
        const uint32_t bytes = r.format == RenderFormat::R32G32_FLOAT ? 8 : 4;
        backend.swap.depth_dump_row_ = (r.width + 63) & ~63u;
        backend.swap.depth_dump_ = backend.device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(backend.swap.depth_dump_row_) * r.height * bytes));
        if (backend.swap.depth_dump_) {
          backend.targets.Transition(r.texture.get(), r.layout, RenderTextureLayout::COPY_SOURCE);
          CopyTextureToBuffer(backend.recorder.L(), backend.swap.depth_dump_.get(), r.texture.get(), r.width, r.height, backend.swap.depth_dump_row_, 0, 0,
                              nullptr);
          backend.targets.Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
          backend.swap.depth_dump_w_ = r.width;
          backend.swap.depth_dump_h_ = r.height;
          backend.swap.depth_dump_format_ = uint32_t(r.format);
          backend.swap.depth_dump_generation_ = r.generation;
          backend.swap.depth_dump_frame_ = r.written_frame;
        }
      }
      const uint32_t fw = uint32_t((uint64_t(fetch.size_2d.width + 1) * r.scale + 3) / 4);
      const uint32_t fh = uint32_t((uint64_t(fetch.size_2d.height + 1) * r.scale + 3) / 4);
      // A depth resolve goes through a view that repeats the depth (see
      // Resolved::swizzled). A colour resolve already holds the channels in
      // the order its fetch swizzle expects; applying it again swapped red
      // and blue across the UI.
      const uint32_t swizzle = uint32_t(fetch.swizzle) & 0xFFF;
      const bool depth_view = r.depth;
      const bool stencil_first = fetch.format == xenos::TextureFormat::k_8_8_8_8;
      if (fw < r.width || fh < r.height) {
        texture_descriptor[slot] = backend.targets.FittedResolved(r, std::min(fw, r.width), std::min(fh, r.height));
        if (depth_view && texture_descriptor[slot]) texture_descriptor[slot] = backend.targets.SwizzledResolved(r, true, swizzle, stencil_first);
      } else {
        backend.targets.Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
        texture_descriptor[slot] = depth_view ? backend.targets.SwizzledResolved(r, false, swizzle, stencil_first) : r.descriptor;
      }
    } else if (fetch.dimension == xenos::DataDimension::k2DOrStacked) {
      backend.textures.fetch_slot_ = int(slot);
      texture_descriptor[slot] = backend.textures.GetGuestTexture(fetch);
      backend.textures.fetch_slot_ = -1;
      if (fetch.stacked) stack_descriptor[slot] = backend.textures.GetGuestStackedTexture(fetch);
    } else if (fetch.dimension == xenos::DataDimension::kCube) {
      cube_descriptor[slot] = backend.textures.GetGuestCubeTexture(fetch);
    } else if (fetch.dimension == xenos::DataDimension::k3D) {
      // tfetch3D reads the same table as a stack's.
      stack_descriptor[slot] = backend.textures.GetGuestVolumeTexture(fetch);
    }
  }

  if (skip_shader_ && (vs->entry->hash == skip_shader_ || (ps && ps->entry->hash == skip_shader_))) {
    return true;
  }
  if (backend.swap.arm_) {
    const uint64_t ps_hash = ps ? ps->entry->hash : 0;
    if (arm_target_) {
      if ((vs->entry->hash == arm_target_ || ps_hash == arm_target_) && backend.swap.arm_new_.empty()) {
        backend.swap.arm_new_.emplace_back(vs->entry->hash, ps_hash);
      }
    } else if (backend.swap.arm_pairs_.insert(vs->entry->hash * 0x9E3779B97F4A7C15ull ^ ps_hash).second && !backend.swap.arm_learn_) {
      backend.swap.arm_new_.emplace_back(vs->entry->hash, ps_hash);
    }
  }
  if (trace_) {
    std::string line = fmt::format("draw #{} vs={:016X} ps={:016X} prim={} n={} mode={} pitch={} h={} cc={:X} cm={:X} bm={:X}",
                                   draw_index,
                                   vs->entry->hash, ps ? ps->entry->hash : 0, uint32_t(prim_type),
                                   index_count, uint32_t(edram_mode), pitch, height,
                                   regs[XE_GPU_REG_RB_COLORCONTROL], color_mask, backend.lists.bin_mask_);
    if (packet_predicated_) line += " pred";
    for (uint32_t i = 0; i < color_count; ++i) {
      reg::RB_COLOR_INFO info;
      info.value = regs[kColorInfo[i]];
      line += fmt::format(" rt{}={:X}/f{}", i, uint32_t(info.color_base), uint32_t(info.color_format));
    }
    if (depth) {
      line += fmt::format(" ds={:X}/f{} z={}{}{} f{}", uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base),
                          uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format),
                          depth_control.z_enable ? "t" : "", depth_control.z_write_enable ? "w" : "",
                          depth_control.stencil_enable ? "s" : "", uint32_t(depth_control.zfunc));
    }
    for (uint32_t slot = 0; slot < 32; ++slot) {
      xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
      if (fetch.type != xenos::FetchConstantType::kTexture || !fetch.base_address) continue;
      uint32_t t_base, t_mip, t_top, t_last;
      gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &t_base,
                                                          &t_mip, &t_top, &t_last);
      line += fmt::format(
          " t{}={:08X}/f{}/{}x{}/d{}/L{}-{}/c{}{}{}/f{}{}{}/a{}/b{}{}{}/sw{:03X}", slot,
          fetch.base_address << 12,
          uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
          uint32_t(fetch.dimension), t_top, t_last, uint32_t(fetch.clamp_x),
          uint32_t(fetch.clamp_y), uint32_t(fetch.clamp_z), uint32_t(fetch.mag_filter),
          uint32_t(fetch.min_filter), uint32_t(fetch.mip_filter), uint32_t(fetch.aniso_filter),
          int32_t(fetch.lod_bias),
          fetch.stacked ? fmt::format("/s{}", uint32_t(fetch.size_2d.stack_depth) + 1) : "",
          FindResolved(fetch.base_address << 12) ? "/R" : "", uint32_t(fetch.swizzle) & 0xFFF);
    }
    Trace("{}", line);
    Trace("  depthcontrol={:08X} stencilrefmask={:08X} bf={:08X} sqconst vs={:08X} ps={:08X} bool={:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}",
          regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_STENCILREFMASK],
          regs[XE_GPU_REG_RB_STENCILREFMASK_BF], regs[XE_GPU_REG_SQ_VS_CONST], regs[XE_GPU_REG_SQ_PS_CONST],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 1],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 2], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 3],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 4], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 5],
          regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 6], regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 7]);
    Trace("  raster mode={:08X} clip={:08X} offset=({},{},{},{}) blend={:08X} constants=({},{},{},{}) alpha={}",
          regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL], regs[XE_GPU_REG_PA_CL_CLIP_CNTL],
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET), regs[XE_GPU_REG_RB_BLENDCONTROL0],
          regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
          regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
          regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF));
    // What a stacked fetch is actually handed: the pixel shader constants,
    // among them the one whose Z picks the slice.
    if (ps) {
      bool stacked = false;
      for (uint32_t slot = 0; slot < 32; ++slot) {
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
        stacked = stacked || (fetch.type == xenos::FetchConstantType::kTexture && fetch.stacked &&
                              fetch.base_address);
      }
      if (stacked) {
        std::string constants;
        for (uint32_t i = 0; i < 4; ++i) {
          constants += fmt::format(" c{}=({},{},{},{})", i,
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 0),
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 1),
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 2),
                                   regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + 3));
        }
        Trace("  ps{:016X}{}", ps->entry->hash, constants);
      }
      // A material that samples the shadow cascades (depth resolves in slots
      // 13-15) picks one by view depth, so the split shows up on the ground as
      // a straight screen line. Its constants, once per shader and frame, give
      // where the splits fall and how each cascade is addressed.
      bool cascades = false;
      for (uint32_t slot = 13; slot < 16; ++slot) {
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
        const auto format = xenos::TextureFormat(uint32_t(fetch.format));
        cascades = cascades || (fetch.type == xenos::FetchConstantType::kTexture && fetch.base_address &&
                                (format == xenos::TextureFormat::k_24_8 ||
                                 format == xenos::TextureFormat::k_24_8_FLOAT ||
                                 format == xenos::TextureFormat::k_16_FLOAT));
      }
      // <files>/plume-trace-ps names one more pixel shader (hex) whose
      // constants go into the trace at every draw, band by band.
      const bool chosen = trace_ps_ && ps->entry->hash == trace_ps_;
      if (chosen || (cascades && backend.swap.traced_cascade_ps_.insert(ps->entry->hash).second)) {
        std::string constants;
        for (uint32_t i = 0; i < 256; ++i) {
          float c[4];
          for (uint32_t j = 0; j < 4; ++j) c[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + j);
          if (!c[0] && !c[1] && !c[2] && !c[3]) continue;
          constants += fmt::format(" c{}=({},{},{},{})", i, c[0], c[1], c[2], c[3]);
        }
        Trace("  psconst ps{:016X}{}", ps->entry->hash, constants);
      }
    }
    const auto* declared = g_shaderInputElements + vs->entry->inputElementOffset;
    for (uint32_t i = 0; i < vs->entry->inputElementCount; ++i) {
      const auto& e = declared[i];
      if (size_t(e.fetchAddress) * 3 + 2 >= vs->ucode_dword_count()) continue;
      const auto* f = vs->ucode_dwords() + size_t(e.fetchAddress) * 3;
      Trace("  fetch L{} @{} {:08X} {:08X} {:08X} exp={}", e.location, e.fetchAddress,
            f[0], f[1], f[2], int32_t(f[1] << 2) >> 26);
    }
  }

  // Constants: the guest register file, exactly as the recompiled shaders
  // address it (vertex c0..c255, pixel c256..c511 as their c0..c255).
  // The block is assembled here and only goes into the ring once it is whole,
  // so that a draw whose constants did not change can keep the last one.
  // Not cleared between draws: every field a shader reads is either written
  // for each draw or only read by the draws that write it (the live fetches,
  // exports, point sizes and clip planes), and what an earlier draw left in the
  // others keeps the second half the same from draw to draw.
  uint8_t* sh = shared_built_;
  // plume-skip bit 16777216 points every fetch at the 1x1 white texture in
  // slot 0: the sampling instructions stay, the texture bandwidth goes.
  if (backend.skip_mask_ & 16777216) {
    std::memset(texture_descriptor, 0, sizeof(texture_descriptor));
    std::memset(cube_descriptor, 0, sizeof(cube_descriptor));
    std::memset(stack_descriptor, 0, sizeof(stack_descriptor));
  }
  // plume-debug-shader mode 16 points every 2D fetch but slot 0 at white, 32
  // only slot 0: which texture of a draw takes its output away.
  if (debug_shader_ && (vs->entry->hash == debug_shader_ || (ps && ps->entry->hash == debug_shader_))) {
    if (debug_mode_ & 16) {
      for (uint32_t i = 1; i < 32; ++i) texture_descriptor[i] = 0;
    }
    if (debug_mode_ & 32) texture_descriptor[0] = 0;
  }
  for (uint32_t mask = backend.swap.white_slots_; mask; mask &= mask - 1) texture_descriptor[__builtin_ctz(mask)] = 0;
  std::memcpy(sh + RENUT_SHARED_TEXTURE2D_OFFSET, texture_descriptor, sizeof(texture_descriptor));
  std::memcpy(sh + RENUT_SHARED_TEXTURECUBE_OFFSET, cube_descriptor, sizeof(cube_descriptor));
  std::memcpy(sh + RENUT_SHARED_TEXTURE3D_OFFSET, stack_descriptor, sizeof(stack_descriptor));
  std::memcpy(sh + RENUT_SHARED_SAMPLER_OFFSET, sampler_descriptor, sizeof(sampler_descriptor));
  std::memcpy(sh + RENUT_SHARED_BOOLEANS_OFFSET, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              RENUT_BOOL_REGISTER_COUNT / 8);
  if (loop_constants_dirty_) {
    loop_constants_dirty_ = false;
    for (uint32_t i = 0; i < RENUT_LOOP_CONSTANT_COUNT; ++i) {
      uint32_t v = regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
      int32_t loop[4] = {int32_t(v & 0xFF), int32_t((v >> 8) & 0xFF), int32_t(int8_t((v >> 16) & 0xFF)), 0};
      std::memcpy(sh + RENUT_SHARED_LOOP_OFFSET + i * 16, loop, 16);
    }
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const bool alpha_test_enabled = color_control.alpha_test_enable &&
                                  color_control.alpha_func != xenos::CompareFunction::kAlways;
  // Alpha to coverage (N&B's grass and foliage) has no samples to cover
  // without MSAA; like a single-sample A2C, keep the fragments at or above
  // half coverage.
  const bool alpha_to_mask = color_control.alpha_to_mask_enable && !(backend.skip_mask_ & 4096);
  bool alpha_test = ps && (alpha_test_enabled || alpha_to_mask);
  float alpha_ref = alpha_test_enabled ? regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF) : 0.5f;
  // The pixel shaders only clip(alpha - ref), i.e. pass at alpha >= ref. N&B's
  // alpha tests are GREATER (97% of the traced ones, mostly against 0) and
  // GREATER_EQUAL, so GREATER takes the next float up: against 0 the shader
  // used to keep the fully transparent texels (the world portal's photo).
  // Not the denormal after 0, which the GPU may flush to zero.
  if (alpha_test_enabled && color_control.alpha_func == xenos::CompareFunction::kGreater) {
    alpha_ref = std::max(std::nextafter(alpha_ref, INFINITY), FLT_MIN);
  }
  std::memcpy(sh + RENUT_SHARED_MISC_OFFSET + 12, &alpha_ref, 4);

  // Vertex inputs: the layout the bound vertex shader's vfetches describe
  // (DecodeLiveElements); the streams they name are the guest's vertex fetch
  // constants 95 - stream.
  // Kept between draws so their storage is not allocated again every draw.
  std::vector<RenderInputElement>& input_elements = draw_input_elements_;
  std::vector<RenderInputSlot>& input_slots = draw_input_slots_;
  input_elements.clear();
  input_slots.clear();
  std::array<uint64_t, 17> slot_offsets{};
  std::array<RenderBuffer*, 17> slot_buffers{};
  std::array<uint32_t, 17> slot_sizes{};
  std::array<bool, 17> slot_used{};
  uint32_t swapped[7] = {};  // texcoord, normal, binormal, tangent, blendweight, position, blendindices
  uint32_t input_remap[RENUT_INPUT_REMAP_COUNT] = {};
  uint32_t packed_inputs = 0, int_inputs = 0;
  bool r11g11b10_normals = false;
  uint64_t streams[RENUT_VERTEX_STREAM_COUNT] = {};
  for (uint32_t stream = 0; stream < RENUT_VERTEX_STREAM_COUNT; ++stream) {
    // The last entry is fetch constant 0, which Direct3D's own Clear uses.
    xenos::xe_gpu_vertex_fetch_t fetch =
        regs.GetVertexFetch(stream == RENUT_DIRECT_FETCH_STREAM ? 0 : 95 - stream);
    if (fetch.type != xenos::FetchConstantType::kVertex) continue;
    uint64_t offset;
    RenderBuffer* buffer;
    Trace("  stream{} {:08X}+{:X} e{}", stream, fetch.address << 2, fetch.size << 2, uint32_t(fetch.endian));
    if (trace_ && fetch.size && (fetch.size << 2) <= (1u << 20)) {
      const auto* guest = backend.memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2);
      backend.swap.traced_streams_.push_back({backend.swap.frame_draw_ - 1, stream, uint32_t(fetch.address << 2),
                                 std::vector<uint8_t>(guest, guest + (fetch.size << 2))});
    }
    // A stream with format 17 vectors the input assembler hands to the shader
    // (see RepackFormat17) is copied for this draw with those moved into the
    // layout the shader decodes.
    if (!vs->repack_decoded && vs->compiled) {
      vs->repack_decoded = true;
      for (const LiveElement& e : vs->elements) {
        if (e.format != 17 || e.slot >= 17 || !e.stride || vs->repack[e.slot].size() == 8) continue;
        for (const auto& [location, integer] : vs->compiled->inputs) {
          if (location == e.location && !integer) {
            vs->repack[e.slot].emplace_back(e.offset, e.stride);
            vs->repack_signature[e.slot] =
                (vs->repack_signature[e.slot] ^ ((uint64_t(e.offset) << 32) | e.stride)) * 0x100000001B3ull + 1;
            break;
          }
        }
      }
    }
    const bool repackable = !patch_list && stream != RENUT_DIRECT_FETCH_STREAM && vs->compiled &&
                            !vs->repack[stream].empty();
    // What the last draw found for this stream is good again when its fetch
    // constant, the native stream window and the repack are the same and no
    // kept buffer went away, the ring has not wrapped, and no export range
    // overlaps (exports change under draws).
    const BufferRange native_stream =
        stream < native_streams_.size() ? native_streams_[stream] : BufferRange{};
    StreamMemo& memo = stream_memo_[stream];
    const uint32_t* constant_words = reinterpret_cast<const uint32_t*>(&fetch);
    const uint64_t signature = repackable ? vs->repack_signature[stream] : 0;
    bool bound = false;
    if (memo.buffer && memo.epoch == stream_epoch_ && memo.generation == upload_generation_ &&
        memo.words[0] == constant_words[0] && memo.words[1] == constant_words[1] &&
        memo.native_base == native_stream.base &&
        memo.native_size == native_stream.size && memo.repack_signature == signature &&
        memo.export_epoch == export_epoch_ && !validate_windows_ &&
        !sync_draws_ && !trace_) {
      buffer = memo.buffer;
      offset = memo.offset;
      bound = memo.bound;
      ++backend.stats_.stream_memo_hits;
    } else if (PooledStream* pooled = backend.textures.objkeys_ && !validate_windows_ && !sync_draws_ && !trace_
                                          ? &stream_pool_[((uint64_t(constant_words[0]) * 0x9E3779B97F4A7C15ull) ^
                                                           (uint64_t(constant_words[1]) * 0xC2B2AE3D27D4EB4Full) ^
                                                           native_stream.base ^ signature) %
                                                          kStreamPoolSize]
                                          : nullptr;
               pooled && pooled->frame == backend.swaps_ && pooled->memo.buffer && pooled->memo.epoch == stream_epoch_ &&
               pooled->memo.generation == upload_generation_ && pooled->memo.words[0] == constant_words[0] &&
               pooled->memo.words[1] == constant_words[1] && pooled->memo.native_base == native_stream.base &&
               pooled->memo.native_size == native_stream.size && pooled->memo.repack_signature == signature &&
               pooled->memo.export_epoch == export_epoch_) {
      // Another slot, or an earlier pass, resolved this same stream this frame.
      memo = pooled->memo;
      buffer = memo.buffer;
      offset = memo.offset;
      bound = memo.bound;
      ++backend.stats_.stream_memo_hits;
    } else {
      bool repacked = false;
      if (repackable && !backend.streams.ExportOverlaps(fetch.address << 2, fetch.size << 2)) {
        buffer = upload_.get();
        repacked = backend.streams.RepackedStream(fetch.address << 2, fetch.size << 2, fetch.endian, vs->repack[stream].data(),
                                  uint32_t(vs->repack[stream].size()), buffer, offset);
        if (repacked) Trace("  stream{} repacked: {} format 17 element(s)", stream, vs->repack[stream].size());
      }
      backend.streams.stream_object_ = native_stream;
      bound = repacked || backend.streams.BindStream(fetch.address << 2, fetch.size << 2, fetch.endian, buffer, offset);
      backend.streams.stream_object_ = {};
      if (!backend.streams.exports_.empty() && backend.streams.ExportOverlaps(fetch.address << 2, fetch.size << 2)) {
        memo.buffer = nullptr;  // an exported stream is not remembered
      } else {
        std::memcpy(memo.words, constant_words, sizeof(memo.words));
        memo.native_base = native_stream.base;
        memo.native_size = native_stream.size;
        memo.repack_signature = signature;
        memo.epoch = stream_epoch_;
        memo.export_epoch = export_epoch_;
        memo.generation = upload_generation_;
        memo.buffer = bound ? buffer : nullptr;
        memo.offset = offset;
        memo.bound = bound;
        if (backend.textures.objkeys_ && memo.buffer && !validate_windows_ && !sync_draws_ && !trace_) {
          PooledStream& pooled = stream_pool_[((uint64_t(constant_words[0]) * 0x9E3779B97F4A7C15ull) ^
                                               (uint64_t(constant_words[1]) * 0xC2B2AE3D27D4EB4Full) ^
                                               native_stream.base ^ signature) %
                                              kStreamPoolSize];
          pooled.memo = memo;
          pooled.frame = backend.swaps_;
        }
      }
    }
    if (bound) {
      streams[stream] = buffer == upload_.get() ? Address(offset) : buffer->getDeviceAddress() + offset;
      slot_offsets[stream] = offset;
      slot_buffers[stream] = buffer;
      slot_sizes[stream] = fetch.size << 2;
    }
  }
  std::memcpy(sh + RENUT_SHARED_STREAMS_OFFSET, streams, sizeof(streams));
  if (validate_windows_) {
    for (uint32_t s = 0; s < RENUT_VERTEX_STREAM_COUNT; ++s) {
      if (!slot_sizes[s] || !slot_buffers[s]) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(s == RENUT_DIRECT_FETCH_STREAM ? 0 : 95 - s);
      backend.resolve.ValidateRange(fmt::format("stream{}", s).c_str(), vs->entry->hash, slot_buffers[s], slot_offsets[s],
                    fetch.address << 2, slot_sizes[s], fetch.endian);
    }
  }
  if (sync_draws_) {
    // What this draw hands the GPU, kept for the line a failure prints.
    std::string info = fmt::format("vs={:016X} ps={:016X} index_count={} streams:", vs->entry->hash,
                                   ps ? ps->entry->hash : 0, index_count);
    for (uint32_t s = 0; s < slot_sizes.size(); ++s) {
      if (!slot_sizes[s]) continue;
      info += fmt::format(" {}:{}B/{}@{:X}", s, slot_sizes[s], slot_buffers[s] == upload_.get() ? "ring" : "kept",
                          s < RENUT_VERTEX_STREAM_COUNT ? streams[s] : 0);
    }
    for (uint32_t s = 0; s < RENUT_VERTEX_STREAM_COUNT; ++s) {
      if (s == RENUT_DIRECT_FETCH_STREAM || !slot_sizes[s] || slot_sizes[s] > 256) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - s);
      std::vector<uint8_t> bytes(slot_sizes[s]);
      CopySwapped(bytes.data(), backend.memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2), slot_sizes[s],
                  fetch.endian);
      info += fmt::format(" data{}:", s);
      for (uint32_t i = 0; i + 4 <= bytes.size() && i < 64; i += 4) {
        float f;
        std::memcpy(&f, bytes.data() + i, 4);
        info += fmt::format(" {:.3g}", f);
      }
    }
    // Whether each kept window still holds what the guest holds: the GPU reads
    // the window, the checks below read the guest. A window the watch missed
    // would give the shader another mesh's bytes.
    for (uint32_t s = 0; s < RENUT_VERTEX_STREAM_COUNT; ++s) {
      if (s == RENUT_DIRECT_FETCH_STREAM || !slot_sizes[s] || !slot_buffers[s] || slot_buffers[s] == upload_.get()) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - s);
      std::vector<uint8_t> guest(slot_sizes[s]);
      CopySwapped(guest.data(), backend.memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2), slot_sizes[s], fetch.endian);
      const auto* window = static_cast<const uint8_t*>(slot_buffers[s]->map());
      if (!window) continue;
      uint32_t differ = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < guest.size(); ++i) {
        if (window[slot_offsets[s] + i] != guest[i]) {
          if (first == UINT32_MAX) first = i;
          ++differ;
        }
      }
      slot_buffers[s]->unmap();
      if (differ) {
        info += fmt::format(" STALE{}[{}B differ of {}, first at {}]", s, differ, guest.size(), first);
        ++backend.recorder.sync_stale_streams_;
      }
    }
    // What the integer attributes hold over the vertices this draw names: a
    // blend index past the palette is a read past the stream, which the Mali
    // faults on (the stream is read through a device address).
    if (!patch_list && vs->compiled) {
      std::vector<uint32_t> used;
      const bool indexed = index_buffer_info && index_buffer_info->guest_base;
      if (indexed) {
        const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
        std::vector<uint8_t> raw(size_t(index_count) * (is32 ? 4 : 2));
        CopySwapped(raw.data(), backend.memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base),
                    uint32_t(raw.size()), index_buffer_info->endianness);
        for (uint32_t i = 0; i < index_count; ++i) {
          used.push_back(is32 ? reinterpret_cast<const uint32_t*>(raw.data())[i]
                              : reinterpret_cast<const uint16_t*>(raw.data())[i]);
        }
      } else {
        for (uint32_t i = 0; i < index_count && i < 65536; ++i) used.push_back(i);
      }
      uint32_t max_vertex = 0;
      for (uint32_t v : used) max_vertex = std::max(max_vertex, v);
      const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
      info += fmt::format(" max_index={} base={} palette={}", max_vertex, base_vertex, slot_sizes[10] / 64);
      for (const LiveElement& e : vs->elements) {
        if (e.normalized || !e.stride || (e.format != 6 && e.format != 25 && e.format != 26)) continue;
        xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - e.slot);
        if (fetch.size == 0 || (fetch.size << 2) > (4u << 20)) continue;
        std::vector<uint8_t> stream(fetch.size << 2);
        CopySwapped(stream.data(), backend.memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2),
                    uint32_t(stream.size()), fetch.endian);
        const uint32_t width = e.format == 6 ? 1 : 2;
        const uint32_t count = e.format == 26 ? 4 : (e.format == 25 ? 2 : 4);
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        uint32_t outside = 0;
        for (uint32_t v : used) {
          const int64_t at = (int64_t(v) + base_vertex) * e.stride + e.offset;
          if (at < 0 || uint64_t(at) + count * width > stream.size()) {
            ++outside;
            continue;
          }
          for (uint32_t c = 0; c < count; ++c) {
            const uint8_t* p = stream.data() + at + c * width;
            int32_t value;
            if (width == 1) {
              value = e.is_signed ? int32_t(int8_t(p[0])) : int32_t(p[0]);
            } else {
              uint16_t w;
              std::memcpy(&w, p, 2);
              value = e.is_signed ? int32_t(int16_t(w)) : int32_t(w);
            }
            lo = std::min(lo, value);
            hi = std::max(hi, value);
          }
        }
        info += fmt::format(" L{}[f{} {}..{} outside={}]", e.location, e.format, lo, hi, outside);
      }
    }
    backend.recorder.sync_draw_info_ = std::move(info);
  }

  // Memory export. What a shader exports into is named by the stream
  // constants it was handed - Direct3D's own layout, recognisable by its bit
  // pattern (see renutMemExport) - and each of those streams becomes an
  // exported range. The shader stores only into the ranges listed here.
  if (vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT) {
    uint32_t ranges[RENUT_EXPORT_RANGE_COUNT * 4] = {};
    uint32_t range_count = 0;
    const uint32_t* constants = &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X];
    // Only the constants the shader builds its addresses from: the register
    // file still holds the stream constants of earlier draws, and a range made
    // from one of those would shadow guest memory the CPU is still writing.
    const uint32_t flags = vs->entry->specConstantsMask;
    const uint32_t last = std::min<uint32_t>(RENUT_SHADER_EXPORT_LAST(flags), 255);
    for (uint32_t i = RENUT_SHADER_EXPORT_FIRST(flags); i <= last && range_count < RENUT_EXPORT_RANGE_COUNT;
         ++i) {
      const uint32_t* k = constants + i * 4;
      if ((k[0] >> 30) != 1 || (k[1] >> 23) != 0x96 || (k[2] >> 23) != 0x96 || (k[3] >> 23) != 0x96) {
        continue;
      }
      const uint32_t element_bytes = ExportElementBytes((k[2] >> 8) & 0x3F);
      const uint32_t elements = k[3] & 0x7FFFFF;
      const uint32_t base = k[0] << 2;
      if (!element_bytes || !elements || base >= 0x20000000u ||
          uint64_t(elements) * element_bytes > (32u << 20)) {
        continue;
      }
      Trace("  eA c{} {:08X} {:08X} {:08X} {:08X} -> {:08X}+{:X} f{}", i, k[0], k[1], k[2], k[3], base,
            elements * element_bytes, (k[2] >> 8) & 0x3F);
      const uint32_t endian_bits = k[2] & 7;
      ExportRange* range = backend.streams.GetExport(base, elements * element_bytes,
                                     endian_bits <= 3 ? xenos::Endian(endian_bits) : xenos::Endian::k8in32);
      if (!range) continue;
      if (range->read) backend.streams.ExportBarrier();
      range->written = true;
      range->exported_frame = backend.swaps_;
      bool listed = false;
      for (uint32_t r = 0; r < range_count; ++r) listed |= ranges[r * 4] == range->base;
      if (listed) continue;
      ranges[range_count * 4 + 0] = range->base;
      ranges[range_count * 4 + 1] = range->size;
      ranges[range_count * 4 + 2] = uint32_t(range->address);
      ranges[range_count * 4 + 3] = uint32_t(range->address >> 32);
      ++range_count;
    }
    std::memcpy(sh + RENUT_SHARED_EXPORT_OFFSET, ranges, sizeof(ranges));
    ++backend.stats_.export_draws;
    ++backend.patches.export_draws_total_;
    if (trace_) {
      std::string line = "  export";
      for (uint32_t r = 0; r < range_count; ++r) {
        line += fmt::format(" {:08X}+{:X}", ranges[r * 4], ranges[r * 4 + 1]);
      }
      Trace("{}", line);
    }
  }
  for (const auto& [k, entry] : patch_list ? vs->domain_live_fetches : vs->live_fetches) {
    std::memcpy(sh + RENUT_SHARED_FETCH_OFFSET + k * 16, entry.data(), 16);
  }
  bool zero_used = false;
  // A patch draw has one input: where in its patch the generated vertex sits.
  // What the guest's shader fetches, it fetches by patch index through the
  // table above.
  if (patch_list) {
    input_elements.emplace_back("TEXCOORD", RENUT_DOMAIN_INPUT_LOCATION,
                                RENUT_DOMAIN_INPUT_LOCATION, RenderFormat::R32G32B32A32_FLOAT,
                                kPatchDomainBinding, 0);
    input_slots.emplace_back(kPatchDomainBinding, uint32_t(4 * sizeof(float)));
  }
  for (auto [location, integer] : vs->compiled->inputs) {
    if (patch_list) break;
    const LiveElement* element = nullptr;
    for (const LiveElement& e : vs->elements) {
      if (e.location == location) {
        element = &e;
        break;
      }
    }
    InputDecode decode = InputDecode::kNone;
    RenderFormat format = element && !integer ? VertexFormat(element->format, element->is_signed,
                                                             element->normalized, decode)
                                              : RenderFormat::UNKNOWN;
    if (!element || format == RenderFormat::UNKNOWN || !element->stride ||
        !slot_sizes[element->slot] ||
        (slot_used[element->slot] &&
         std::find_if(input_slots.begin(), input_slots.end(), [&](const RenderInputSlot& s) {
           return s.index == element->slot && s.stride != element->stride;
         }) != input_slots.end())) {
      input_elements.emplace_back("TEXCOORD", location, location,
                                  integer ? RenderFormat::R32G32B32A32_UINT
                                          : RenderFormat::R32G32B32A32_FLOAT,
                                  kZeroBinding, 0);
      zero_used = true;
      continue;
    }
    input_elements.emplace_back("TEXCOORD", location, location, format, element->slot,
                                element->offset);
    switch (decode) {
      case InputDecode::kInteger: int_inputs |= 1u << location; break;
      case InputDecode::kPacked1010102: packed_inputs |= 1u << location; break;
      case InputDecode::kPacked111110:
        // tfetchR11G11B10 decodes these, for normals, tangents and binormals only.
        r11g11b10_normals |= element->usage == 3 || element->usage == 6 || element->usage == 7;
        break;
      default: break;
    }
    if (!slot_used[element->slot]) {
      slot_used[element->slot] = true;
      input_slots.emplace_back(element->slot, element->stride);
    }
    // After the k8in32 swap the input assembler reads the hardware's component
    // order (X in the low half), and Direct3D's patched vfetch swizzle (.yxwz)
    // puts 16-bit halves back in memory order. A shader recompiled with that
    // swizzle needs nothing more - swapping every 16-bit element here mangled
    // every mesh with half-float positions. One recompiled without it gets the
    // difference remapped (see FetchRemap).
    if (location < RENUT_INPUT_REMAP_COUNT) input_remap[location] = element->remap;
    // Debug: plume-skip bit 512 reverses every 16-bit pair a second time, the
    // old behaviour.
    if (IsSixteenBitFormat(element->format) && (backend.skip_mask_ & 512)) {
      // Debug: the old behaviour, reversing the 16-bit pairs a second time.
      int which = -1;
      switch (element->usage) {
        case 5: which = 0; break;   // TEXCOORD
        case 3: which = 1; break;   // NORMAL
        case 7: which = 2; break;   // BINORMAL
        case 6: which = 3; break;   // TANGENT
        case 1: which = 4; break;   // BLENDWEIGHT
        case 0: which = 5; break;   // POSITION
        case 2: which = 6; break;   // BLENDINDICES
        default: break;
      }
      if (which >= 0) swapped[which] |= 1u << element->usage_index;
    }
  }
  if (sync_draws_ && !patch_list && vs->compiled && sync_diag_[vs->entry->hash]++ < 1) {
    // One line per vertex shader: what its fetches can reach, for finding the
    // draw that reads past a stream (the Mali faults where other GPUs read on).
    std::string diag = fmt::format("Renut: Plume draw diag vs={:016X} ps={:016X} count={} indexed={} streams:",
                                   vs->entry->hash, ps ? ps->entry->hash : 0, index_count,
                                   index_buffer_info && index_buffer_info->guest_base);
    for (uint32_t s = 0; s < slot_sizes.size(); ++s) {
      if (slot_sizes[s]) diag += fmt::format(" {}:{}B@{:X}", s, slot_sizes[s], s < RENUT_VERTEX_STREAM_COUNT ? streams[s] : 0);
    }
    diag += " live:";
    for (const auto& [k, entry] : vs->live_fetches) {
      diag += fmt::format(" #{}(stream {} fmt {} stride {} off {})", k, entry[0] & 0xFF, (entry[0] >> 8) & 0xFF,
                          entry[1], entry[2]);
    }
    // The vertices the draw names, then the extremes of each integer attribute.
    std::vector<uint32_t> used;
    if (index_buffer_info && index_buffer_info->guest_base) {
      const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
      std::vector<uint8_t> raw(size_t(index_count) * (is32 ? 4 : 2));
      CopySwapped(raw.data(), backend.memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base),
                  uint32_t(raw.size()), index_buffer_info->endianness);
      for (uint32_t i = 0; i < index_count; ++i) {
        used.push_back(is32 ? reinterpret_cast<const uint32_t*>(raw.data())[i]
                            : reinterpret_cast<const uint16_t*>(raw.data())[i]);
      }
    } else {
      for (uint32_t i = 0; i < index_count && i < 65536; ++i) used.push_back(i);
    }
    uint32_t max_vertex = 0;
    for (uint32_t v : used) max_vertex = std::max(max_vertex, v);
    diag += fmt::format(" max_index={} base_vertex={} elements:", max_vertex, int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]));
    for (const LiveElement& e : vs->elements) {
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(95 - e.slot);
      diag += fmt::format(" L{}(slot {} fmt {}{} stride {} off {} cap {})", e.location, e.slot, e.format,
                          e.is_signed ? "s" : "", e.stride, e.offset, e.stride ? (fetch.size << 2) / e.stride : 0);
      if (e.format != 26 || e.normalized || !e.stride || fetch.size == 0) continue;
      std::vector<uint8_t> stream(fetch.size << 2);
      CopySwapped(stream.data(), backend.memory_->TranslatePhysical<const uint8_t*>(fetch.address << 2),
                  uint32_t(stream.size()), fetch.endian);
      int32_t lo = INT32_MAX, hi = INT32_MIN;
      for (uint32_t v : used) {
        const uint64_t at = uint64_t(int64_t(v) + int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET])) * e.stride + e.offset;
        if (at + 8 > stream.size()) continue;
        const int16_t* comps = reinterpret_cast<const int16_t*>(stream.data() + at);
        for (int c = 0; c < 4; ++c) {
          lo = std::min<int32_t>(lo, comps[c]);
          hi = std::max<int32_t>(hi, comps[c]);
        }
      }
      diag += fmt::format("[int16 range {}..{}]", lo, hi);
    }
    REXLOG_INFO("{}", diag);
  }
  if (trace_) {
    std::string line = "  inputs";
    for (auto [location, integer] : vs->compiled->inputs) {
      const LiveElement* e = nullptr;
      for (const LiveElement& l : vs->elements) {
        if (l.location == location) e = &l;
      }
      if (!e) {
        line += fmt::format(" L{}{}=none", location, integer ? "u" : "f");
        continue;
      }
      const auto fetch = regs.GetVertexFetch(95 - e->slot);
      line += fmt::format(" L{}{}=u{}.{}/f{}{}{}/s{}+{}/e{}", location, integer ? "u" : "f", e->usage,
                          e->usage_index, e->format, e->is_signed ? "s" : "", e->normalized ? "n" : "",
                          e->stride, e->offset, uint32_t(fetch.endian));
    }
    Trace("{}", line);
    // The first vertices' dwords as the host reads them (already swapped), by
    // stream, and the fetches the shader does on its own.
    for (uint32_t stream = 0; stream < slot_sizes.size(); ++stream) {
      if (!slot_sizes[stream] || slot_buffers[stream] != upload_.get()) continue;
      const auto* words = reinterpret_cast<const uint32_t*>(Pointer(slot_offsets[stream]));
      line = fmt::format("  data{}", stream);
      // All of it for the shader an armed trace is after.
      const uint32_t shown = vs->entry->hash == arm_target_ ? 4096 : 16;
      for (uint32_t i = 0; i < std::min<uint32_t>(shown, slot_sizes[stream] / 4); ++i) {
        line += fmt::format(" {:08X}", words[i]);
      }
      Trace("{}", line);
    }
    line = "  vsc";
    for (uint32_t i = 0; i < 8; ++i) {
      line += fmt::format(" c{}=({},{},{},{})", i, regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 0),
                          regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 1),
                          regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 2),
                          regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + 3));
    }
    Trace("{}", line);
    if (ps && (ps->entry->hash == debug_shader_ || ps->entry->hash == arm_target_)) {
      for (uint32_t i = 0; i < 32; ++i) {
        float c[4];
        for (uint32_t j = 0; j < 4; ++j) c[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + j);
        if (c[0] || c[1] || c[2] || c[3]) Trace("  psc{}=({},{},{},{})", i, c[0], c[1], c[2], c[3]);
      }
    }
    if (vs->entry->hash == debug_shader_ || vs->entry->hash == arm_target_) {
      // Every constant for the shader under study, to run it by hand.
      for (uint32_t i = 8; i < 256; ++i) {
        float c[4];
        for (uint32_t j = 0; j < 4; ++j) c[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + j);
        if (c[0] || c[1] || c[2] || c[3]) Trace("  vsc{}=({},{},{},{})", i, c[0], c[1], c[2], c[3]);
      }
      Trace("  bools {:08X} {:08X}", regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
            regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 1]);
    }
    for (const auto& [k, entry] : patch_list ? vs->domain_live_fetches : vs->live_fetches) {
      float scale;
      std::memcpy(&scale, &entry[3], 4);
      Trace("  live{} stream={} format={} flags={} stride={} remap={:X} offset={} scale={}", k,
            entry[0] & 0xFF, (entry[0] >> 8) & 0xFF, entry[0] >> 16, entry[1] & 0xFFFF, entry[1] >> 16,
            entry[2], scale);
    }
  }
  if (backend.skip_mask_) {
    bool skinned = false, half_position = false;
    for (const LiveElement& e : vs->elements) {
      skinned |= e.usage == 2;
      half_position |= e.usage == 0 && e.format == 32;
    }
    const int32_t base = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
    if (((backend.skip_mask_ & 1) && int_inputs) || ((backend.skip_mask_ & 2) && packed_inputs) ||
        ((backend.skip_mask_ & 4) && skinned) || ((backend.skip_mask_ & 8) && half_position) ||
        ((backend.skip_mask_ & 16) && quad_list) || ((backend.skip_mask_ & 32) && base != 0) ||
        ((backend.skip_mask_ & 64) && !skinned) || ((backend.skip_mask_ & 128) && zero_used)) {
      return true;
    }
  }
  if (backend.swap.skin_log_) {
    bool skinned = false;
    for (const LiveElement& e : vs->elements) skinned |= e.usage == 2;
    if (skinned) {
      std::lock_guard<std::mutex> lock(backend.swap.skinned_mutex_);
      SkinnedSeen& seen = backend.resolve.skinned_seen_[vs->entry->hash];
      if (!seen.draws++) seen.first_frame = backend.swaps_;
      seen.last_frame = backend.swaps_;
      if (backend.swap.skin_skip_.count(vs->entry->hash)) return true;
    }
  }
  if (zero_used) input_slots.emplace_back(kZeroBinding, 0);
  // The input remaps, the swapped, packed and integer input masks go to the
  // shaders as specialization constants, below with the pipeline key.

  // Viewport. With the guest's viewport transform enabled the shader emits
  // clip space and the host viewport reproduces PA_CL_VPORT_*; with it off the
  // shader emits window coordinates and g_ViewportScaleOffset maps them.
  // XenosRecomp compiles with -fvk-invert-y, so the shaders already negate Y:
  // window y = yo + ys * ndc becomes a viewport of height -2 * ys at yo + ys.
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const Target* extent = colors[0] ? colors[0] : depth;
  // Its size in guest pixels: a shadow map is drawn at a fraction of it.
  const uint32_t guest_width = extent ? (extent->width * 4 + extent->scale - 1) / extent->scale : 0;
  const uint32_t guest_height = extent ? (extent->height * 4 + extent->scale - 1) / extent->scale : 0;
  // Window offset (predicated tiling moves each band to the target's origin)
  // and the D3D9 pixel centre at integer coordinates.
  float window_x = 0.f, window_y = 0.f;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    window_x = float(offset.window_x_offset);
    window_y = float(offset.window_y_offset);
  }
  const bool keep_pixel_center =
      backend.swap.pixel_center_debug_ == 0 ||
      (backend.swap.pixel_center_debug_ == 1 && edram_mode != xenos::EdramMode::kDepthOnly);
  if (keep_pixel_center && regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    window_x += 0.5f;
    window_y += 0.5f;
  }
  RenderViewport viewport;
  float scale_offset[4] = {1.f, 1.f, 0.f, 0.f};
  if (vte.vport_x_scale_ena) {
    float xs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
    float xo = vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.f;
    float ys = vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.f;
    float yo = vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.f;
    float zs = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.f;
    float zo = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.f;
    viewport = RenderViewport(xo - xs + window_x, yo + ys + window_y, 2.f * xs, -2.f * ys,
                              std::clamp(zo, 0.f, 1.f), std::clamp(zo + zs, 0.f, 1.f));
  } else {
    viewport = RenderViewport(window_x, window_y, float(guest_width), float(guest_height));
    scale_offset[0] = 2.f / float(guest_width);
    scale_offset[1] = -2.f / float(guest_height);
    scale_offset[2] = -1.f;
    scale_offset[3] = 1.f;
  }
  if (viewport.width <= 0.f) {
    viewport = RenderViewport(0.f, 0.f, float(guest_width), float(guest_height));
  }
  // Debug, split mode 5 (game_fixes.cpp): the band a predicated packet belongs
  // to, for the scissor below. Only predicated packets count - the mask stays
  // set between them. Off by default: with one tile the game predicates nearly
  // everything to mask 3, which then means the whole screen (the particles
  // lost their lower half to this scissor).
  const bool band_split_scene = g_renut_scene_split_mode.load(std::memory_order_relaxed) == 5 &&
                                !REXCVAR_GET(renut_scene_msaa) &&
                                regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 1280;
  const uint32_t draw_bands = band_split_scene && packet_predicated_ ? uint32_t(backend.lists.bin_mask_) & 0xF : 0;
  constexpr float kBandSplit = 384.f;  // 2x MSAA at 720p: bands of 384 and 336 rows
  std::memcpy(sh + RENUT_SHARED_VIEWPORT_OFFSET, scale_offset, sizeof(scale_offset));
  // Pixels that read their position relative to a band (renutFragPos): none
  // but with the debug switch below.
  float band_split = 0.f;
  // Debug (<files>/plume-band-fragpos "1"): with scene MSAA off, pixels below
  // row 384 of the scene read their position relative to the lower band, as
  // the console's lower-band replay gave them.
  if (backend.swap.band_fragpos_ && !REXCVAR_GET(renut_scene_msaa) && regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 1280 &&
      std::fabs(viewport.height) >= 700.f) {
    band_split = 384.f;
  }
  std::memcpy(sh + RENUT_SHARED_BAND_SPLIT_OFFSET, &band_split, sizeof(band_split));

  // User clip planes, in the guest's clip space; the vertex shaders turn the
  // enabled ones into a clip distance (renutUserClipDistance). N&B's
  // quarter-resolution mirrored pass clips at plane 0, and without it the
  // geometry on the far side of the plane filled the buffer the fog reads.
  // Which planes are on is a specialization constant (g_SpecUserClipMask).
  const auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const uint32_t ucp_mask = clip_cntl.clip_disable ? 0u : uint32_t(clip_cntl.ucp_ena);
  if (ucp_mask) {
    for (uint32_t i = 0; i < RENUT_USER_CLIP_PLANE_COUNT; ++i) {
      std::memcpy(sh + RENUT_SHARED_UCP_OFFSET + i * 16, &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 16);
    }
  }

  // Point sprites. The sizes are 12.4 fixed point radii in guest pixels; the
  // shaders want diameters. One unit of host NDC is half the viewport, so a
  // diameter over the viewport's extent is the radius in NDC - signed with the
  // viewport, so that -y is the top of the screen whichever way it is flipped.
  if (point_list) {
    const auto point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    const auto point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const float point[6] = {float(point_size.width) * (2.f / 16.f),
                            float(point_size.height) * (2.f / 16.f),
                            float(point_minmax.min_size) * (2.f / 16.f),
                            float(point_minmax.max_size) * (2.f / 16.f),
                            1.f / viewport.width, 1.f / viewport.height};
    std::memcpy(sh + RENUT_SHARED_POINT_OFFSET, point, sizeof(point));
    Trace("  points size={}x{} min={} max={} ndc/px={}x{} regs {:08X} {:08X}", point[0], point[1], point[2],
          point[3], point[4], point[5], point_size.value, point_minmax.value);
  }

  // The shared block is complete; place all three where the shaders will read
  // them. A block that overflows the ring flushes it, which drops the two
  // placed before it, so the generation says whether to lay them down again.
  // A vertex shader that reads constants by a0/aL (skinning) gets the whole
  // file placed as one block too, when rel_constants_mode_ asks for it. Patch
  // draws run the shader's domain variant, which has its own entry: they keep
  // the four blocks.
  // The refraction sprites (PS A8DCAC79: the swim splash, the wake) look up the
  // scene copy at the screen position plus a distortion, with the row clamped
  // to [c5.y, c5.z] - the part of the screen the tile being drawn covers. With
  // one tile (MSAA off) the game leaves (c5.y, c5.z) = (1, 0): the minimum
  // above the maximum pins every lookup to the top row, and the splash came out
  // as blocks of the sky's blue or the cliff's brown. The bounds are put in
  // order for this draw only; the register goes back afterwards.
  constexpr uint64_t kRefractionSpritePs = 0xA8DCAC7963881F1Cull;
  constexpr uint32_t kRefractionBoundsAt = XE_GPU_REG_SHADER_CONSTANT_256_X + 5 * 4;
  float refraction_saved[4] = {};
  bool refraction_fixed = false;
  if (backend.swap.refraction_fix_ && ps && ps->entry && ps->entry->hash == kRefractionSpritePs &&
      !REXCVAR_GET(renut_scene_msaa)) {
    float* bounds = reinterpret_cast<float*>(&backend.register_file_->values[kRefractionBoundsAt]);
    if (bounds[1] > bounds[2]) {
      std::memcpy(refraction_saved, bounds, sizeof(refraction_saved));
      std::swap(bounds[1], bounds[2]);
      MarkConstantsWritten(kRefractionBoundsAt, 4);
      refraction_fixed = true;
    }
  }
  struct RefractionBoundsRestore {
    Draw* self;
    const float* saved;
    bool active;
    ~RefractionBoundsRestore() {
      if (!active) return;
      std::memcpy(&self->backend.register_file_->values[kRefractionBoundsAt], saved, 4 * sizeof(float));
      self->MarkConstantsWritten(kRefractionBoundsAt, 4);
    }
  } refraction_restore{this, refraction_saved, refraction_fixed};
  uint32_t rel_variant = 0;
  if (rel_constants_mode_ && !patch_list && (vs->entry->specConstantsMask & RENUT_SHADER_FLAG_REL_CONSTANTS)) {
    rel_variant = rel_constants_mode_ < 10
                      ? rel_constants_mode_
                      : ((rel_toggle_[vs->entry->hash & 255]++ & 1) ? rel_constants_mode_ - 10 : 0);
  }
  const bool rel_active = rel_variant != 0;
  uint64_t generation;
  do {
    generation = upload_generation_;
    bool placed = true;
    // Depth-only draws have no fragment shader, so its constants can wait
    // until a color draw actually needs them.
    for (uint32_t stage = 0; stage < (ps || !backend.swap.shadow_native_constants_ ? 2u : 1u) && placed; ++stage) {
      uint8_t* shadow = stage ? ps_shadow_ : vs_shadow_;
      const auto* file = reinterpret_cast<const uint8_t*>(
          &regs.values[stage ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X]);
      for (uint32_t chunk = 0; chunk < kConstantChunks && placed; ++chunk) {
        placed = backend.streams.CommitConstants(shadow + chunk * kConstantChunkSize, file + chunk * kConstantChunkSize,
                                 kConstantChunkSize, constant_offsets_[stage][chunk],
                                 &constants_written_[stage][chunk]);
      }
    }
    if (placed && rel_active) {
      placed = backend.streams.CommitConstants(vs_all_shadow_,
                               reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]),
                               kStageConstantSize, vs_all_offset_, nullptr);
    }
    if (!placed ||
        !backend.streams.CommitConstants(shared_shadow_, shared_built_, RENUT_SHARED_HOT_SIZE, shared_offset_) ||
        !backend.streams.CommitConstants(shared_shadow_ + RENUT_SHARED_HOT_SIZE, shared_built_ + RENUT_SHARED_HOT_SIZE,
                         RENUT_SHARED_SIZE - RENUT_SHARED_HOT_SIZE, shared_cold_offset_)) {
      return backend.shaders.Drop("constants upload", vs, ps);
    }
  } while (generation != upload_generation_);

  if (backend.swap.twin_dedupe_ && packet_predicated_ && !REXCVAR_GET(renut_scene_msaa)) {
    const uint64_t tiles = backend.lists.bin_mask_ & 0xF;
    // Only the draws seen recorded twice: screen-space rectangles (the
    // shadow multiply) and the water's patches. Objects the game splits by
    // band must all run, and the band jobs may draw look-alikes.
    const bool twin_kind = prim_type == xenos::PrimitiveType::kRectangleList ||
                           prim_type == xenos::PrimitiveType::kQuadList || patch_list;
    if (tiles != 0 && tiles != 0xF && twin_kind) {
      uint64_t parts[16];
      uint32_t n = 0;
      parts[n++] = vs->entry->hash;
      parts[n++] = ps ? ps->entry->hash : 0;
      parts[n++] = (uint64_t(uint32_t(prim_type)) << 32) | index_count;
      parts[n++] = index_buffer_info ? (uint64_t(index_buffer_info->guest_base) << 8) |
                                           uint32_t(index_buffer_info->format)
                                     : 0;
      // The float constants each shader reads, by value. Offsets would not do
      // - a block lands at a new place in the ring whenever another draw's
      // constants were placed in between - and neither would whole blocks:
      // registers the shader never reads keep what the draw before left.
      auto used_hash = [](HostShader* shader, const uint8_t* file) -> uint64_t {
        if (!shader) return 0;
        if (!shader->bool_mask_decoded) {
          rex::string::StringBuffer disassembly;
          shader->AnalyzeUcode(disassembly);
          std::memcpy(shader->bool_mask, shader->constant_register_map().bool_bitmap, sizeof(shader->bool_mask));
          shader->bool_mask_decoded = true;
        }
        const auto& map = shader->constant_register_map();
        uint64_t h = 0;
        for (uint32_t block = 0; block < 4; ++block) {
          for (uint64_t bits = map.float_bitmap[block]; bits; bits &= bits - 1) {
            const uint32_t index = block * 64 + uint32_t(__builtin_ctzll(bits));
            h = XXH3_64bits_withSeed(file + index * 16, 16, h);
          }
        }
        return h;
      };
      parts[n++] = used_hash(vs, vs_shadow_);
      parts[n++] = used_hash(ps, ps_shadow_);
      uint64_t sig = XXH3_64bits(parts, n * sizeof(uint64_t));
      // The RB/PA/SC/VGT registers (blend, depth, targets, viewport, scissor,
      // window offset) and the fetch constants (textures, vertex streams).
      sig = XXH3_64bits_withSeed(&regs.values[0x2000], 1024 * sizeof(uint32_t), sig);
      sig = XXH3_64bits_withSeed(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 192 * sizeof(uint32_t), sig);
      bool twin = false;
      for (const TwinEntry& e : twin_ring_) {
        if (e.frame == backend.swaps_ && e.sig == sig && (e.mask & tiles) == 0) {
          twin = true;
          break;
        }
      }
      if (twin) {
        ++backend.stats_.twin_skipped;
        Trace("twin of an earlier draw under mask {:X}, skipped", uint64_t(backend.lists.bin_mask_));
        return true;
      }
      twin_ring_[twin_pos_++ % twin_ring_.size()] = {sig, tiles, backend.swaps_};
    }
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  RenderRect scissor_rect(int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
                          int32_t(scissor.offset[0] + scissor.extent[0]),
                          int32_t(scissor.offset[1] + scissor.extent[1]));
  // Split mode 5 only (see draw_bands): keep a band-predicated packet inside
  // its band.
  if (scissor_rect.bottom >= 720) {
    if (draw_bands == 0x3) {
      scissor_rect.bottom = std::min(scissor_rect.bottom, int32_t(kBandSplit));
      ++backend.stats_.band_widened;
    } else if (draw_bands == 0xC) {
      scissor_rect.top = std::max(scissor_rect.top, int32_t(kBandSplit));
    }
  }
  if (trace_) {
    Trace("  viewport ({}, {}) {}x{} window ({}, {}) scissor ({}, {})-({}, {}) vte={:X}", viewport.x,
          viewport.y, viewport.width, viewport.height, window_x, window_y, scissor_rect.left,
          scissor_rect.top, scissor_rect.right, scissor_rect.bottom, vte.value);
    Trace("  zviewport scale={} offset={}", regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
  }
  if (scissor_rect.right <= scissor_rect.left || scissor_rect.bottom <= scissor_rect.top) {
    return backend.shaders.Drop("empty scissor", vs, ps);
  }

  // Pipeline.
  const auto raster = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const bool polygonal = draw_util::IsPrimitivePolygonal(false, prim_type);
  RenderCullMode cull = RenderCullMode::NONE;
  if (polygonal && !(backend.skip_mask_ & 131072)) {
    if (raster.cull_front && raster.cull_back) return backend.shaders.Drop("culls both faces", vs, ps);
    if (raster.cull_front) cull = RenderCullMode::FRONT;
    else if (raster.cull_back) cull = RenderCullMode::BACK;
  }
  const RenderFrontFace front_face = raster.face ? RenderFrontFace::CLOCKWISE
                                                : RenderFrontFace::COUNTER_CLOCKWISE;
  const bool debug_draw =
      debug_shader_ && (vs->entry->hash == debug_shader_ || (ps && ps->entry->hash == debug_shader_));
  if (debug_draw && (debug_mode_ & 4)) {
    cull = RenderCullMode::NONE;
    alpha_test = false;
  }
  const bool depth_enabled =
      depth && depth_control.z_enable && !(backend.skip_mask_ & 1024) && !(debug_draw && (debug_mode_ & 1));
  const bool depth_write = depth_enabled && depth_control.z_write_enable;
  // Polygon offset, converted like the Xenia Vulkan path does: the constant is
  // a fraction of the depth range, times 2^24 for a float depth target (Vulkan
  // counts it in units of the smallest depth step), the slope is in 1/16 of a
  // pixel. The guest's depth is the one stored (reversed through the viewport),
  // so a positive offset moves toward the camera here too.
  int32_t depth_bias = 0;
  float depth_slope = 0.0f;
  if (backend.swap.poly_offset_ && depth_enabled) {
    float slope_scale = 0.0f, offset = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope_scale, offset);
    const float bias = offset * 16777216.0f * backend.swap.poly_offset_scale_;
    if (std::isfinite(bias)) depth_bias = int32_t(std::clamp(bias, -2.0e9f, 2.0e9f));
    const float slope = slope_scale * xenos::kPolygonOffsetScaleSubpixelUnit * backend.swap.poly_offset_scale_;
    if (std::isfinite(slope)) depth_slope = slope;
  }
  // Stencil: the game marks pixels in the stencil buffer and later draws shade
  // only where they are marked; without the test those draws covered their
  // whole volume. Plume takes one reference and one pair of masks for both
  // faces, so the back face shares the front's.
  const bool stencil_enabled = depth && depth_control.stencil_enable;
  const auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
  auto stencil_face = [](xenos::CompareFunction func, xenos::StencilOp fail, xenos::StencilOp zpass,
                         xenos::StencilOp zfail) {
    // RenderStencilOp lists the Xenos operations in order, after UNKNOWN.
    auto op = [](xenos::StencilOp o) { return RenderStencilOp(uint32_t(o) + 1); };
    RenderStencilFaceDesc face;
    face.compareFunction = Compare(func);
    face.failOp = op(fail);
    face.passOp = op(zpass);
    face.depthFailOp = op(zfail);
    return face;
  };
  RenderStencilFaceDesc stencil_front, stencil_back;
  if (stencil_enabled) {
    stencil_front = stencil_face(depth_control.stencilfunc, depth_control.stencilfail,
                                 depth_control.stencilzpass, depth_control.stencilzfail);
    stencil_back = depth_control.backface_enable
                       ? stencil_face(depth_control.stencilfunc_bf, depth_control.stencilfail_bf,
                                      depth_control.stencilzpass_bf, depth_control.stencilzfail_bf)
                       : stencil_front;
  }
  RenderBlendDesc blends[4];
  for (uint32_t i = 0; i < color_count; ++i) {
    reg::RB_BLENDCONTROL bc;
    bc.value = regs[kBlendControl[i]];
    RenderBlendDesc& b = blends[i];
    b.srcBlend = Blend(bc.color_srcblend);
    b.dstBlend = Blend(bc.color_destblend);
    b.blendOp = BlendOp(bc.color_comb_fcn);
    b.srcBlendAlpha = Blend(bc.alpha_srcblend);
    b.dstBlendAlpha = Blend(bc.alpha_destblend);
    b.blendOpAlpha = BlendOp(bc.alpha_comb_fcn);
    b.blendEnabled = IsBlendable(colors[i]->format) &&
                     !(b.srcBlend == RenderBlend::ONE && b.dstBlend == RenderBlend::ZERO &&
                       b.blendOp == RenderBlendOperation::ADD &&
                       b.srcBlendAlpha == RenderBlend::ONE && b.dstBlendAlpha == RenderBlend::ZERO &&
                       b.blendOpAlpha == RenderBlendOperation::ADD);
    b.renderTargetWriteMask = uint8_t((color_mask >> (4 * i)) & 0xF);
    if (debug_draw && (debug_mode_ & 2)) {
      b.blendEnabled = false;
      b.renderTargetWriteMask = 0xF;
    }
    // renut_plume_no_bloom: the bright pass (scene * its alpha + glow) writes
    // black, so every blur after it and the final add are black too.
    if (backend.no_bloom_ && ps && ps->entry->hash == 0x1B958C7DC860BBBEull && IsBlendable(colors[i]->format)) {
      b.blendEnabled = true;
      b.srcBlend = b.dstBlend = b.srcBlendAlpha = b.dstBlendAlpha = RenderBlend::ZERO;
      b.blendOp = b.blendOpAlpha = RenderBlendOperation::ADD;
    }
  }
  // Blend constants (RB_BLEND_RED..ALPHA) are dynamic state, set per draw, in
  // the pipelines whose factors use them. Plume left them at zero, so the
  // game's glow draws - wand sparkles, holograms - added no alpha for bloom.
  bool blend_constants = false;
  for (uint32_t i = 0; i < color_count; ++i) {
    const RenderBlendDesc& b = blends[i];
    auto constant = [](RenderBlend f) { return f == RenderBlend::BLEND_FACTOR || f == RenderBlend::INV_BLEND_FACTOR; };
    blend_constants |= b.blendEnabled && (constant(b.srcBlend) || constant(b.dstBlend) ||
                                          constant(b.srcBlendAlpha) || constant(b.dstBlendAlpha));
  }
  const uint32_t spec = (alpha_test ? SPEC_CONSTANT_ALPHA_TEST : 0) |
                        (r11g11b10_normals ? SPEC_CONSTANT_R11G11B10_NORMAL : 0);
  // Specialization constants (shader_common.h): 0 the flags above, 1 packed
  // inputs, 2 integer inputs, 3 user clip planes, 4..10 swapped inputs by usage,
  // 16 + location each input's remap. Zero is every constant's default, so
  // only the others are listed - and all of them go into the key.
  std::vector<RenderSpecConstant>& spec_constants = draw_spec_constants_;
  spec_constants.clear();
  spec_constants.emplace_back(0, spec);
  if (packed_inputs) spec_constants.emplace_back(1, packed_inputs);
  if (int_inputs) spec_constants.emplace_back(2, int_inputs);
  if (ucp_mask) spec_constants.emplace_back(3, ucp_mask);
  for (uint32_t i = 0; i < 7; ++i) {
    if (swapped[i]) spec_constants.emplace_back(4 + i, swapped[i]);
  }
  for (uint32_t i = 0; i < RENUT_SPEC_INPUT_REMAP_LOCATIONS; ++i) {
    if (input_remap[i]) spec_constants.emplace_back(RENUT_SPEC_INPUT_REMAP_BASE + i, input_remap[i]);
  }
  // The boolean registers the two shaders branch on, and only those: a
  // register neither tests stays out of the key, so it cannot multiply the
  // pipelines of shaders that ignore it.
  auto bools_of = [](HostShader* shader) -> const uint32_t* {
    if (!shader) return nullptr;
    if (!shader->bool_mask_decoded) {
      rex::string::StringBuffer disassembly;
      shader->AnalyzeUcode(disassembly);
      std::memcpy(shader->bool_mask, shader->constant_register_map().bool_bitmap,
                  sizeof(shader->bool_mask));
      shader->bool_mask_decoded = true;
    }
    return shader->bool_mask;
  };
  // A shader that reads them from the shared block adds none.
  const CompiledShader* vs_module = patch_list ? vs->domain_compiled : vs->compiled;
  const uint32_t* vs_bools = vs_module && vs_module->dynamic_bools ? nullptr : bools_of(vs);
  const uint32_t* ps_bools = ps && ps->compiled && ps->compiled->dynamic_bools ? nullptr : bools_of(ps);
  for (uint32_t word = 0; word < 8 && (vs_bools || ps_bools); ++word) {
    const uint32_t tested = (vs_bools ? vs_bools[word] : 0) | (ps_bools ? ps_bools[word] : 0);
    const uint32_t value = regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + word] & tested;
    if (value) spec_constants.emplace_back(RENUT_SPEC_BOOLS_BASE + word, value);
  }
  // The live vertex fetch table, if the shader's module has a form that reads
  // it from specialization constants (spirv_rewrites.h): what the table holds
  // comes from the shader's own vfetch instructions, so it is the same on every
  // draw that uses this shader and adds no pipelines of its own.
  CompiledShader* vertex_compiled = patch_list ? vs->domain_compiled : vs->compiled;
  const bool fetch_spec = backend.swap.fetch_spec_mode_ && vertex_compiled && vertex_compiled->fetch_spec_module;
  // The table is the same on every draw of the shader. With key reuse the key
  // takes one hash of it (made once, kept on the shader) and the constants
  // themselves are laid into spec_constants only when a pipeline has to be
  // made - at the position they had, so the driver's cache sees the same list.
  const auto& fetch_table = patch_list ? vs->domain_live_fetches : vs->live_fetches;
  uint64_t fetch_table_hash = 0;
  if (fetch_spec) {
    if (backend.swap.key_reuse_) {
      uint64_t& cached = vs->fetch_spec_hash[patch_list ? 1 : 0];
      if (!cached) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (const auto& [slot, entry] : fetch_table) {
          for (uint32_t word = 0; word < 4; ++word) {
            if (entry[word]) {
              const uint32_t pair[2] = {kFetchSpecBase + slot * 4 + word, entry[word]};
              h = XXH3_64bits_withSeed(pair, sizeof(pair), h);
            }
          }
        }
        cached = h | 1;  // never 0: that is "not made"
      }
      fetch_table_hash = cached;
    } else {
      for (const auto& [slot, entry] : fetch_table) {
        for (uint32_t word = 0; word < 4; ++word) {
          if (entry[word]) spec_constants.emplace_back(kFetchSpecBase + slot * 4 + word, entry[word]);
        }
      }
    }
  }
  if (rel_active) spec_constants.emplace_back(RENUT_SPEC_REL_CONSTANTS, rel_variant);
  // The pipeline key: the fields laid end to end and hashed once. Hashing
  // each field on its own was a few dozen XXH3 calls a draw.
  KeyBytes key_bytes;
  key_bytes.Put(vs->compiled);
  key_bytes.Put(ps ? ps->compiled : nullptr);
  key_bytes.Put(topology);
  key_bytes.Put(uint8_t(rect_list | (point_list << 1) | (patch_list << 2)));
  key_bytes.Put(cull);
  key_bytes.Put(front_face);
  key_bytes.Put(color_count);
  for (uint32_t i = 0; i < color_count; ++i) {
    key_bytes.Put(colors[i]->format);
    key_bytes.Put(blends[i].blendEnabled);
    key_bytes.Put(blends[i].srcBlend);
    key_bytes.Put(blends[i].dstBlend);
    key_bytes.Put(blends[i].blendOp);
    key_bytes.Put(blends[i].srcBlendAlpha);
    key_bytes.Put(blends[i].dstBlendAlpha);
    key_bytes.Put(blends[i].blendOpAlpha);
    key_bytes.Put(blends[i].renderTargetWriteMask);
  }
  key_bytes.Put(uint8_t((depth != nullptr) | (depth_enabled << 1) | (depth_write << 2)));
  key_bytes.Put(depth_control.zfunc);
  // Only stencil draws add bytes, so the cached pipelines of every other draw
  // keep their keys. The reference and masks are dynamic: the game changes
  // them from draw to draw, and keying on them made thousands of pipelines.
  if (stencil_enabled) key_bytes.Put(uint32_t(depth_control.value & 0xFFFFFF81u));
  if (REXCVAR_GET(renut_plume_decal_log) && (raster.poly_offset_front_enable || raster.poly_offset_back_enable ||
                                              raster.poly_offset_para_enable)) {
    // Every draw that asks for a polygon offset (ground decals, shadow maps):
    // each distinct state once, with the raw register values and what Vulkan got,
    // and a count of them every 300 frames.
    float log_slope = 0.0f, log_offset = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, log_slope, log_offset);
    ++bias_log_draws_;
    if (depth_bias != 0 || depth_slope != 0.0f) ++bias_log_applied_;
    uint64_t sig = 0;
    HashCombine(sig, vs->entry->hash);
    HashCombine(sig, ps ? ps->entry->hash : 0);
    HashCombine(sig, uint32_t(depth_control.value));
    HashCombine(sig, raster.value);
    HashCombine(sig, uint32_t(depth_bias));
    HashCombine(sig, uint32_t(blends[0].blendEnabled));
    if (bias_log_seen_.insert(sig).second) {
      REXLOG_INFO(
          "Renut: Plume offset draw vs={:016X} ps={:016X} prim={} indices={} zfunc={} (0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always) zwrite={} depth={} blend={} "
          "alpha_test={} colors={} cull={} slope={:g} offset={:g} -> bias={} slope={:g} (poly offset {} x{})",
          vs->entry->hash, ps ? ps->entry->hash : 0, uint32_t(prim_type), index_count, uint32_t(depth_control.zfunc),
          depth_write, depth_enabled, blends[0].blendEnabled, alpha_test, color_count, uint32_t(cull), log_slope,
          log_offset, depth_bias, depth_slope, backend.swap.poly_offset_ ? "on" : "off",
          backend.swap.poly_offset_scale_);
    }
    if (backend.swaps_ >= bias_log_next_) {
      bias_log_next_ = backend.swaps_ + 300;
      REXLOG_INFO("Renut: Plume offset draws so far: {} asked, {} with a non-zero bias, {} distinct states",
                  bias_log_draws_, bias_log_applied_, bias_log_seen_.size());
    }
  }
  // Likewise only biased draws add bytes.
  if (depth_bias != 0 || depth_slope != 0.0f) {
    key_bytes.Put(depth_bias);
    key_bytes.Put(depth_slope);
  }
  // Where the boolean registers' values sit in the key, so the family key can
  // blank them.
  std::array<size_t, 8> bool_spans;
  uint32_t bool_span_count = 0;
  for (const RenderSpecConstant& c : spec_constants) {
    key_bytes.Put(c.index);
    if (c.index >= RENUT_SPEC_BOOLS_BASE && c.index < RENUT_SPEC_BOOLS_BASE + 8 && bool_span_count < 8) {
      bool_spans[bool_span_count++] = key_bytes.size;
    }
    key_bytes.Put(c.value);
  }
  if (fetch_table_hash) key_bytes.Put(fetch_table_hash);
  // Geometry-shader and patch draws keep their own: their stages expect the
  // guest's outputs.
  const bool plain_draw = !rect_list && !point_list && !patch_list;
  // A draw with no fragment shader reads none of the vertex shader's outputs
  // but its position, so it can have the form that does not write them
  // (spirv_rewrites.h).
  RenderShader* depth_module = nullptr;
  if (backend.swap.depth_strip_mode_ && !ps && plain_draw && vertex_compiled) {
    depth_module = fetch_spec ? vertex_compiled->depth_fetch_spec_module.get() : vertex_compiled->depth_module.get();
  }
  const bool depth_strip = depth_module != nullptr;
  key_bytes.Put(uint8_t(fetch_spec | (depth_strip << 1)));
  for (const auto& e : input_elements) {
    key_bytes.Put(e.location);
    key_bytes.Put(e.format);
    key_bytes.Put(e.slotIndex);
    key_bytes.Put(e.alignedByteOffset);
  }
  for (const auto& s : input_slots) {
    key_bytes.Put(s.index);
    key_bytes.Put(s.stride);
  }
  const uint64_t key = key_bytes.Hash();
  // What a pipeline must agree on with the draw for another one to stand in
  // for it without faulting: attachments, primitive and vertex input. Only the
  // plume-skip bit below uses it.
  uint64_t interface_key = 0;
  if (backend.skip_mask_ & 262144) {
    HashCombine(interface_key, topology);
    HashCombine(interface_key, rect_list);
    HashCombine(interface_key, point_list);
    HashCombine(interface_key, patch_list);
    HashCombine(interface_key, color_count);
    for (uint32_t i = 0; i < color_count; ++i) HashCombine(interface_key, colors[i]->format);
    HashCombine(interface_key, depth ? depth->format : RenderFormat::UNKNOWN);
    for (const auto& e : input_elements) {
      HashCombine(interface_key, e.location);
      HashCombine(interface_key, e.format);
      HashCombine(interface_key, e.slotIndex);
      HashCombine(interface_key, e.alignedByteOffset);
    }
    for (const auto& s : input_slots) {
      HashCombine(interface_key, s.index);
      HashCombine(interface_key, s.stride);
    }
  }
  std::unique_ptr<RenderPipeline>* pipeline_node = pipeline_memo_.Find(key);
  if (!pipeline_node) {
    pipeline_node = &backend.pipelines.pipelines_[key];
    pipeline_memo_.Put(key, pipeline_node);
  }
  auto& pipeline = *pipeline_node;
  RenderPipeline* standin = nullptr;
  if (!pipeline) {
    if (fetch_table_hash) {
      // The table the key stood for, ahead of the relative-constants entry where
      // the list had it.
      auto at = spec_constants.end() - (rel_active ? 1 : 0);
      std::vector<RenderSpecConstant> table;
      for (const auto& [slot, entry] : fetch_table) {
        for (uint32_t word = 0; word < 4; ++word) {
          if (entry[word]) table.emplace_back(kFetchSpecBase + slot * 4 + word, entry[word]);
        }
      }
      spec_constants.insert(at, table.begin(), table.end());
    }
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = backend.layout_.get();
    desc.vertexShader = depth_strip ? depth_module
                        : fetch_spec ? vertex_compiled->fetch_spec_module.get()
                                     : vertex_compiled->module.get();
    desc.geometryShader = rect_list ? rect_list_gs_.get() : point_list ? point_list_gs_.get() : nullptr;
    desc.pixelShader = ps ? ps->compiled->module.get() : nullptr;
    desc.primitiveTopology = topology;
    desc.cullMode = cull;
    desc.frontFace = front_face;
    desc.depthClipEnabled = true;
    desc.depthEnabled = depth_enabled;
    desc.depthWriteEnabled = depth_write;
    desc.depthFunction = depth_enabled ? Compare(depth_control.zfunc) : RenderComparisonFunction::ALWAYS;
    desc.depthTargetFormat = depth ? depth->format : RenderFormat::UNKNOWN;
    desc.depthBias = depth_bias;
    desc.slopeScaledDepthBias = depth_slope;
    desc.stencilEnabled = stencil_enabled;
    if (stencil_enabled) {
      desc.stencilFrontFace = stencil_front;
      desc.stencilBackFace = stencil_back;
    }
    desc.renderTargetCount = color_count;
    for (uint32_t i = 0; i < color_count; ++i) {
      desc.renderTargetFormat[i] = colors[i]->format;
      desc.renderTargetBlend[i] = blends[i];
    }
    desc.inputElements = input_elements.data();
    desc.inputElementsCount = uint32_t(input_elements.size());
    desc.inputSlots = input_slots.data();
    desc.inputSlotsCount = uint32_t(input_slots.size());
    desc.specConstants = spec_constants.data();
    desc.specConstantsCount = uint32_t(spec_constants.size());
    // The pipeline built ahead from the recipe file (StartRecipes), if this
    // start had it; a pipeline not in the file yet is written to it.
    if (backend.pipelines.recipes_ && !pending_pipelines_.count(key) &&
        !failed_pipelines_.count(key)) {
      Recipe recipe;
      recipe.vs = (patch_list ? vs->domain_entry : vs->entry)->hash;
      recipe.ps = ps ? ps->entry->hash : 0;
      recipe.domain = patch_list;
      recipe.form = depth_strip ? (fetch_spec ? kRecipeDepthFetchSpec : kRecipeDepth)
                                : (fetch_spec ? kRecipeFetchSpec : kRecipeModule);
      recipe.gs = rect_list ? 1 : point_list ? 2 : 0;
      recipe.desc = desc;
      recipe.elements = input_elements;
      if (!backend.pipelines.predict_desc_set_[ps ? 0 : 1] && !patch_list && !rect_list && !point_list) {
        backend.pipelines.predict_desc_[ps ? 0 : 1] = desc;
        backend.pipelines.predict_desc_set_[ps ? 0 : 1] = true;
      }
      recipe.slots = input_slots;
      recipe.constants = spec_constants;
      if (backend.lists.draw_pass_ && !patch_list && !rect_list && !point_list) {
        backend.pipelines.LearnPassState(backend.lists.draw_pass_, recipe);
      }
      pipeline = backend.pipelines.TakeRecipe(recipe, key);
      if (pipeline) {
        ++backend.stats_.pipelines;
        ++backend.stats_.pipelines_prebuilt;
      }
    }
    // Exporting shaders feed later draws and sometimes the guest CPU. Never
    // skip those draws merely because their first pipeline is still compiling.
    if (!pipeline && REXCVAR_GET(renut_plume_async_pipelines) &&
        !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) {
      // A draw whose pipeline is still compiling is drawn with a ready sibling
      // (same shaders, targets, blending and vertex layout; other boolean
      // registers) when there is one. Dropping it instead left holes in the
      // scene for as long as the compile took, and the screen flickered.
      auto use_standin = [&](uint64_t family) {
        auto found = standins_.find(family);
        if (found == standins_.end()) return false;
        standin = found->second;
        ++backend.report.drops_[{"pipeline stand-in", vs->entry->hash, ps ? ps->entry->hash : 0, false}];
        return true;
      };
      // A draw that goes without its pipeline says so to the job, which
      // decides what the workers take first (TakeUrgentJob): more for a
      // colour draw than a shadow caster, and more for a large mesh than a
      // particle, so walls and floors come before the effects around them.
      auto missed = [&](PipelineJob& job) {
        const uint32_t weight = (ps ? 4u : 1u) * (1u + std::min<uint32_t>(index_count, 65535u) / 512u);
        job.demand.fetch_add(weight, std::memory_order_relaxed);
        job.wanted_frame.store(backend.swaps_, std::memory_order_relaxed);
      };
      auto pending = pending_pipelines_.find(key);
      if (pending != pending_pipelines_.end()) {
        if (!pending->second->done.load(std::memory_order_acquire)) {
          missed(*pending->second);
          if (!use_standin(pending->second->family)) return backend.shaders.Drop("pipeline compiling", vs, ps);
        } else {
          backend.pipelines.CountJob(*pending->second);
          pipeline = std::move(pending->second->result);
          if (pipeline) standins_[pending->second->family] = pipeline.get();
          pending_pipelines_.erase(pending);
          if (!pipeline) {
            failed_pipelines_.insert(key);
            REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})",
                        vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0);
          }
        }
      } else if (!failed_pipelines_.count(key)) {
        KeyBytes family_bytes = key_bytes;
        for (uint32_t i = 0; i < bool_span_count; ++i) {
          std::memset(family_bytes.data + bool_spans[i], 0, sizeof(RenderSpecConstant::value));
        }
        const uint64_t family = family_bytes.Hash();
        const uint64_t program = backend.pipelines.ProgramKey(desc);
        // Most new pipelines differ from one already built only in blending,
        // depth, targets or vertex layout, which the driver does not compile
        // for: their program is in its cache - from this run or an earlier one
        // - and a lookup only links them. Asked here, within a budget per
        // frame, the draw is drawn now instead of a frame or more later. Not
        // for a program a worker has yet to finish: the cache cannot have it.
        bool looked_up = false;
        if (lookup_here_ && lookup_spent_us_ < lookup_budget_us_ && !backend.pipelines.pending_programs_.count(program)) {
          const auto lookup_start = std::chrono::steady_clock::now();
          {
            PlumePipelineLookupOnly only;
            pipeline = CreateGraphicsPipeline(backend.device_, desc);
          }
          const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - lookup_start).count());
          lookup_spent_us_ += us;
          backend.stats_.lookup_us += us;
          looked_up = true;
          if (pipeline) {
            ++backend.stats_.pipelines;
            ++backend.stats_.pipelines_found_here;
            backend.stats_.pipeline_us += us;
            standins_[family] = pipeline.get();
            // A lookup that takes a compile's time is a driver that compiled
            // anyway; on this thread that is the stall the workers exist for.
            if (us > 40000 && backend.swaps_ > 300 && ++slow_lookups_ >= 5) {
              lookup_here_ = false;
              REXLOG_WARN("Renut: Plume pipeline lookups at the draw compiled instead; left to the workers");
            }
          } else {
            ++backend.stats_.lookups_missed;
          }
        }
        if (!pipeline) {
          // Past the limit the draw waits for a free slot rather than compiling
          // here: a stall on the command list thread is what this option avoids.
          if (pending_pipelines_.size() >= 4096) return backend.shaders.Drop("pipeline queue full", vs, ps);
          auto job = std::make_shared<PipelineJob>();
          job->desc = desc;
          job->elements = input_elements;
          job->slots = input_slots;
          job->constants = spec_constants;
          job->family = family;
          job->program = program;
          job->looked_up = looked_up;
          job->vs_hash = vs->ucode_data_hash();
          job->ps_hash = ps ? ps->ucode_data_hash() : 0;
          const bool covered = use_standin(family);
          job->urgent = !covered;
          missed(*job);
          backend.pipelines.StartPipelineWorker();
          ++backend.pipelines.pending_programs_[program];
          pending_pipelines_.emplace(key, job);
          {
            std::lock_guard lock(backend.pipelines.pipeline_mutex_);
            (!looked_up && backend.pipelines.lookup_on_workers_ ? backend.pipelines.pipeline_lookup_queue_
             : covered                        ? backend.pipelines.pipeline_queue_
                                              : backend.pipelines.pipeline_urgent_queue_).push_back(std::move(job));
          }
          backend.pipelines.pipeline_ready_.notify_all();
          if (!covered) return backend.shaders.Drop("pipeline compiling", vs, ps);
        }
      }
    }
    if (!pipeline && !standin && !failed_pipelines_.count(key) && !pending_pipelines_.count(key)) {
      const auto pipeline_start = std::chrono::steady_clock::now();
      pipeline = CreateGraphicsPipeline(backend.device_, desc);
      backend.stats_.pipeline_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - pipeline_start)
                                         .count());
      ++backend.stats_.pipelines;
    }
    if (pipeline && getenv("RENUT_PIPELINE_STATS")) LogPipelineStatistics(*pipeline, vs, ps);
    if (pipeline && getenv("RENUT_PIPELINE_IR")) {
      DumpPipelineInternals(*pipeline, vs, ps, backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/')));
    }
    if (!pipeline && !standin) {
      REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})",
                  vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0);
    }
  }
  if (!pipeline && !standin) {
    ++backend.stats_.bad_shader;
    return true;
  }
  if (backend.pipelines.recipes_ && pipeline) backend.pipelines.NoteSceneUse(key);

  // Indices.
  uint32_t vertex_count = index_count;
  bool indexed = index_buffer_info && index_buffer_info->guest_base;
  RenderFormat index_format = RenderFormat::R16_UINT;
  uint64_t index_offset = 0;
  RenderBuffer* index_buffer = upload_.get();
  uint32_t index_bytes = 0;
  const PatchGrid* patch_grid = nullptr;
  AdaptiveDraw adaptive_draw;
  bool adaptive = false;
  if (patch_list) {
    // Every three control points are a patch - four for a quad patch - and
    // each is expanded into a grid of level * level triangles over the
    // triangle's own coordinates, or level * level quads over the quad's (see
    // GetPatchGrid). The guest's per-edge factors live where the indices would
    // be, computed by a draw that exports them to memory; the level is still
    // the ceiling the guest set for this draw.
    const uint32_t patches = index_count / (quad_patch ? 4 : 3);
    const float wanted = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    uint32_t level = std::clamp(uint32_t(wanted > 1.0f ? wanted : 1.0f), 1u, kMaxPatchLevel);
    // files/plume-patchlevel: a share of the guest's level, in percent. The
    // grid has level^2 cells, so half the level is a quarter of the vertices.
    if (backend.swap.patch_level_percent_ < 100) level = std::max(1u, level * backend.swap.patch_level_percent_ / 100);
    indexed = false;
    if (!patches) {
      ++backend.stats_.skipped_prim;
      return true;
    }
    // In adaptive mode the "indices" are the factors, three per triangle
    // patch, which the guest exported just before: those patches are cut per
    // edge on the GPU, most of them - all that are off screen - into a single
    // triangle. Quad patches stay on the grid.
    if (backend.swap.adaptive_mode_ && !quad_patch &&
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive && index_buffer_info &&
        index_buffer_info->guest_base && index_buffer_info->format == xenos::IndexFormat::kInt32) {
      const float least = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
      const uint32_t min_level = std::clamp(uint32_t(least > 1.0f ? least : 1.0f), 1u, level);
      adaptive = backend.patches.ExpandAdaptivePatches(index_buffer_info->guest_base, index_buffer_info->endianness, patches,
                                       min_level, level, adaptive_draw);
    }
    if (!adaptive) {
      patch_grid = backend.streams.GetPatchGrid(quad_patch, level, patches);
      if (!patch_grid) return backend.shaders.Drop("patch grid", vs, ps);
      vertex_count = patches * patch_grid->indices_per_patch;
    } else {
      vertex_count = 0;
    }
  }
  if (indexed) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    index_format = is32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT;
    const uint32_t size = index_count * (is32 ? 4 : 2);
    // A quad list's indices are read back here to be split into triangles,
    // so they stay in the ring.
    backend.streams.stream_object_ = backend.lists.native_indices_;
    const bool uploaded =
        quad_list ? backend.streams.UploadGuest(index_buffer_info->guest_base, size, index_buffer_info->endianness, index_offset)
                  : backend.streams.GuestStream(index_buffer_info->guest_base, size, index_buffer_info->endianness,
                                index_buffer, index_offset);
    backend.streams.stream_object_ = {};
    if (!uploaded) return backend.shaders.Drop("index upload", vs, ps);
    if (validate_windows_ && !quad_list) {
      backend.resolve.ValidateRange("index", vs->entry->hash, index_buffer, index_offset, index_buffer_info->guest_base, size,
                    index_buffer_info->endianness);
    }
    index_bytes = size;
  }
  if (quad_list) {
    // Two triangles per quad, as 32-bit indices.
    const uint32_t quads = index_count / 4;
    uint64_t offset = backend.streams.Allocate(uint64_t(quads) * 6 * 4, 256);
    if (offset == UINT64_MAX) return backend.shaders.Drop("upload full", vs, ps);
    auto* out = reinterpret_cast<uint32_t*>(Pointer(offset));
    for (uint32_t q = 0; q < quads; ++q) {
      uint32_t v[4];
      for (uint32_t k = 0; k < 4; ++k) {
        uint32_t i = q * 4 + k;
        if (!indexed) v[k] = i;
        else if (index_format == RenderFormat::R32_UINT)
          v[k] = reinterpret_cast<const uint32_t*>(Pointer(index_offset))[i];
        else
          v[k] = reinterpret_cast<const uint16_t*>(Pointer(index_offset))[i];
      }
      const uint32_t tri[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
      std::memcpy(out + q * 6, tri, sizeof(tri));
    }
    indexed = true;
    index_format = RenderFormat::R32_UINT;
    index_buffer = upload_.get();
    index_offset = offset;
    index_bytes = quads * 6 * 4;
    vertex_count = quads * 6;
  }

  // Record. What the Vulkan calls need goes into a packet first. A draw that
  // cannot change the picture when it is reordered joins a window and is
  // recorded, grouped by pipeline, when the window closes; any other draw
  // closes the window and is recorded at once. Nothing from here on reads the
  // register file for the draw's sake, so it can wait.
  RenderFramebuffer* framebuffer = backend.targets.GetFramebuffer(colors, color_count, depth);
  if (!framebuffer) {
    ++backend.stats_.no_target;
    return true;
  }
  DrawPacket pk;
  pk.framebuffer = framebuffer;
  pk.pass_colors = backend.targets.color_count_of_pass_;
  pk.pass_depth = backend.targets.depth_of_pass_;
  pk.pipeline = standin ? standin : pipeline.get();
  pk.interface_key = interface_key;
  pk.vs_hash = vs->entry->hash ^ (uint64_t(rel_constants_mode_ >= 10 ? rel_variant : 0) << 60);
  pk.ps_hash = ps ? ps->entry->hash : 0;
  if (backend.swap.trail_) {
    pk.s0_addr = streams[0];
    pk.s0_size = slot_sizes[0];
    pk.s10_addr = streams[10];
    pk.s10_size = slot_sizes[10];
  }
  if (backend.swap.trail_) {
    for (uint32_t i = 0; i < RENUT_VERTEX_STREAM_COUNT; ++i) {
      pk.stream_addr[i] = streams[i];
      pk.stream_size[i] = slot_sizes[i];
    }
  }
  if (blend_constants) {
    pk.blend_constants = true;
    pk.blend_factor[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    pk.blend_factor[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    pk.blend_factor[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    pk.blend_factor[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }
  if (stencil_enabled) {
    pk.stencil = true;
    pk.stencil_ref = stencil_ref_mask.stencilref;
    pk.stencil_mask = stencil_ref_mask.stencilmask;
    pk.stencil_write_mask = stencil_ref_mask.stencilwritemask;
  }
  // Inactive fragment bindings still need a valid offset. Keep their values
  // uncommitted so the next draw with a fragment shader uploads them.
  const bool skip_ps_constants = !ps && backend.swap.shadow_native_constants_;
  const uint64_t unused_ps_offset = backend.targets.slice_base_;
  pk.push[0] = Address(constant_offsets_[0][0]);
  pk.push[1] = Address(skip_ps_constants ? unused_ps_offset : constant_offsets_[1][0]);
  pk.push[2] = Address(shared_offset_);
  for (uint32_t stage = 0; stage < 2; ++stage) {
    for (uint32_t chunk = 0; chunk < kConstantChunks; ++chunk) {
      pk.constant_offsets[stage * kConstantChunks + chunk] =
          uint32_t(stage == 1 && skip_ps_constants ? unused_ps_offset : constant_offsets_[stage][chunk]);
    }
  }
  pk.constant_offsets[kSharedHotBinding] = uint32_t(shared_offset_);
  pk.constant_offsets[kSharedColdBinding] = uint32_t(shared_cold_offset_);
  // Draws that read no constant by a0/aL keep whatever whole-file block was
  // placed last (any valid offset will do, nothing reads it), so they do not
  // rebind the set.
  pk.constant_offsets[kRelBinding] = pk.constant_offsets[kRelBinding + 1] =
      uint32_t(vs_all_offset_ != UINT64_MAX ? vs_all_offset_ : unused_ps_offset);
  pk.viewport = viewport;
  // plume-skip bit 8388608 scissors every draw to one pixel: vertex work and
  // binning stay, fragment work goes. What is left is the per-draw cost.
  if (backend.skip_mask_ & 8388608) scissor_rect = RenderRect(0, 0, 1, 1);
  // A shadow map drawn at a fraction of the guest's resolution (see Target).
  if (extent && extent->scale != 4) {
    const float fraction = float(extent->scale) / 4.f;
    pk.viewport.x *= fraction;
    pk.viewport.y *= fraction;
    pk.viewport.width *= fraction;
    pk.viewport.height *= fraction;
    auto low = [&](int32_t v) { return int32_t(HostFloor(extent, uint32_t(std::max(v, 0)))); };
    auto high = [&](int32_t v) { return int32_t(HostCeil(extent, uint32_t(std::max(v, 0)))); };
    scissor_rect = RenderRect(std::min(low(scissor_rect.left), int32_t(extent->width)),
                              std::min(low(scissor_rect.top), int32_t(extent->height)),
                              std::min(high(scissor_rect.right), int32_t(extent->width)),
                              std::min(high(scissor_rect.bottom), int32_t(extent->height)));
  }
  pk.scissor = scissor_rect;
  draw_slots_.clear();
  for (const auto& slot : input_slots) {
    DeferredSlot entry;
    entry.slot = slot;
    if (adaptive) {
      entry.view = RenderVertexBufferView(RenderBufferReference(adaptive_draw.buffer, adaptive_draw.points_offset),
                                          uint32_t(adaptive_draw.points_bytes));
    } else if (patch_list) {
      entry.view = RenderVertexBufferView(RenderBufferReference(patch_grid->points.get(), 0),
                                          patch_grid->patches * patch_grid->points_per_patch * 4 *
                                              uint32_t(sizeof(float)));
    } else if (slot.index == kZeroBinding) {
      entry.view = RenderVertexBufferView(RenderBufferReference(upload_.get(), backend.zero_offset_), 64);
    } else {
      entry.view = RenderVertexBufferView(RenderBufferReference(slot_buffers[slot.index], slot_offsets[slot.index]),
                                          slot_sizes[slot.index]);
    }
    draw_slots_.push_back(entry);
  }
  pk.slot_count = uint32_t(draw_slots_.size());
  const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
  if (trace_ && index_buffer == upload_.get()) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < vertex_count; ++i) {
      uint32_t v = !indexed ? i
                   : index_format == RenderFormat::R32_UINT
                       ? reinterpret_cast<const uint32_t*>(Pointer(index_offset))[i]
                       : reinterpret_cast<const uint16_t*>(Pointer(index_offset))[i];
      if (indexed && v == (index_format == RenderFormat::R32_UINT ? 0xFFFFFFFFu : 0xFFFFu)) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    std::string line = fmt::format("  indices {}..{} base={} count={}", lo, hi, base_vertex, vertex_count);
    for (const LiveElement& e : vs->elements) {
      if (e.usage != 0 || !e.stride || !slot_sizes[e.slot] || slot_buffers[e.slot] != upload_.get()) continue;
      const uint32_t capacity = slot_sizes[e.slot] / e.stride;
      line += fmt::format(" pos(slot {} fmt {} cap {})", e.slot, e.format, capacity);
      // Decode the referenced positions as the host will read them.
      float mn = 1e30f, mx = -1e30f;
      uint32_t bad = 0;
      for (uint32_t v = lo; v <= hi && v - lo < 4096; ++v) {
        const uint64_t vi = uint64_t(int64_t(v) + base_vertex);
        const uint64_t at = vi * e.stride + e.offset;
        if (at + 16 > slot_sizes[e.slot]) { ++bad; continue; }
        const uint8_t* p = Pointer(slot_offsets[e.slot]) + at;
        for (int c = 0; c < 3; ++c) {
          float f;
          if (e.format == 32) {
            uint16_t h;
            std::memcpy(&h, p + 2 * (c ^ 1), 2);  // pairs reversed by the 32-bit swap
            const uint32_t sign = h >> 15, exp = (h >> 10) & 31, mant = h & 1023;
            f = exp == 31 ? INFINITY : exp ? std::ldexp(float(mant | 1024), int(exp) - 25)
                                           : std::ldexp(float(mant), -24);
            if (sign) f = -f;
          } else {
            std::memcpy(&f, p + 4 * c, 4);
          }
          if (!std::isfinite(f)) ++bad;
          else { mn = std::min(mn, f); mx = std::max(mx, f); }
        }
      }
      line += fmt::format(" range [{}, {}] bad={}", mn, mx, bad);
    }
    Trace("{}", line);
  }
  // plume-skip bit 1073741824 records everything but the draw itself: the
  // state, pipeline and constant changes are all that reach the GPU.
  backend.stats_.vertices += vertex_count;
  if (rect_list || point_list) ++backend.stats_.gs_draws;
  if (patch_list) {
    ++backend.stats_.patch_draws;
    backend.stats_.patch_vertices += vertex_count;
  }
  // plume-skip bit 2147483648 submits only each draw's first triangle: the
  // per-draw state and setup stay, the vertex work goes.
  const uint32_t draw_count = (backend.skip_mask_ & 2147483648u) ? std::min(vertex_count, 3u) : vertex_count;
  if (backend.skip_mask_ & 1073741824) {
    pk.kind = DrawPacket::Kind::kNone;
  } else if (indexed) {
    // The ring is bound whole and each draw starts at its own indices, which
    // Allocate aligns to 256 bytes - one binding for every draw of the list.
    const uint32_t index_size = index_format == RenderFormat::R32_UINT ? 4 : 2;
    pk.kind = DrawPacket::Kind::kIndexed;
    pk.index_buffer = index_buffer;
    pk.index_format = index_format;
    pk.count = draw_count;
    pk.first = uint32_t(index_offset / index_size);
    pk.base_vertex = base_vertex;
  } else if (adaptive) {
    pk.kind = DrawPacket::Kind::kPatchIndirect;
    pk.index_buffer = adaptive_draw.buffer;
    pk.index_format = RenderFormat::R32_UINT;
    pk.indirect_offset = adaptive_draw.args_offset;
  } else if (patch_list) {
    pk.kind = DrawPacket::Kind::kPatch;
    pk.index_buffer = patch_grid->indices.get();
    pk.index_format = RenderFormat::R32_UINT;
    pk.count = draw_count;
  } else {
    pk.kind = DrawPacket::Kind::kPlain;
    pk.count = draw_count;
    pk.first = uint32_t(std::max(base_vertex, 0));
  }

  // Which draws may wait. Two opaque draws that test and write depth in a
  // strict direction leave the nearer fragment at every pixel whichever one is
  // recorded first - only an exact depth tie could differ - provided they share
  // what a passing fragment writes: the attachments, the colour mask and the
  // depth and stencil state. Stencil reorders only when it never reads and
  // writes on depth pass alone, since then the surviving value is the nearest
  // surface's either way. Depth-only draws (the shadow maps) are exactly
  // independent; with mode 2 opaque colour draws join them. Draws that
  // export to memory, patches, merged bands and every debug switch stay in
  // order.
  bool defer = false;
  uint64_t window_signature = 0;
  if (backend.swap.defer_mode_ && !sync_draws_ && !trace_ && !(backend.skip_mask_ & ~kStateOnlySkipBits) && !backend.recorder.bind_skip_ && !debug_draw && !patch_list &&
      !(vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT) && depth_write) {
    const xenos::CompareFunction zf = depth_control.zfunc;
    const bool strict = zf == xenos::CompareFunction::kLess || zf == xenos::CompareFunction::kLessEqual ||
                        zf == xenos::CompareFunction::kGreater || zf == xenos::CompareFunction::kGreaterEqual;
    auto stencil_free = [](xenos::CompareFunction func, xenos::StencilOp fail, xenos::StencilOp zpass,
                           xenos::StencilOp zfail) {
      return func == xenos::CompareFunction::kAlways && fail == xenos::StencilOp::kKeep &&
             zfail == xenos::StencilOp::kKeep &&
             (zpass == xenos::StencilOp::kKeep || zpass == xenos::StencilOp::kZero ||
              zpass == xenos::StencilOp::kReplace);
    };
    const bool stencil_ok =
        !stencil_enabled ||
        (stencil_free(depth_control.stencilfunc, depth_control.stencilfail, depth_control.stencilzpass,
                      depth_control.stencilzfail) &&
         (!depth_control.backface_enable ||
          stencil_free(depth_control.stencilfunc_bf, depth_control.stencilfail_bf,
                       depth_control.stencilzpass_bf, depth_control.stencilzfail_bf)));
    bool opaque = true;
    for (uint32_t i = 0; i < color_count; ++i) opaque = opaque && !blends[i].blendEnabled;
    // A colour draw with a depth bias is a decal over geometry at the same depth,
    // which is exactly where order decides: it stays in order. Shadow maps use
    // a bias too, and do not care.
    const bool biased = raster.poly_offset_front_enable || raster.poly_offset_back_enable;
    defer = strict && stencil_ok && (!color_pass || (backend.swap.defer_mode_ >= 2 && opaque && !biased));
    if (defer) {
      HashCombine(window_signature, framebuffer);
      HashCombine(window_signature, color_mask);
      HashCombine(window_signature, uint32_t(depth_control.value));
      HashCombine(window_signature, stencil_enabled ? uint32_t(stencil_ref_mask.stencilwritemask) : 0u);
      HashCombine(window_signature, uint32_t(edram_mode));
    }
  }
  if (backend.census.active_ && pk.kind != DrawPacket::Kind::kNone) {
    CensusSignature sig;
    sig.vs = pk.vs_hash;
    sig.ps = pk.ps_hash;
    sig.depth_control = depth_control.value;
    sig.stencil = stencil_ref_mask.value;
    sig.stencil_bf = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
    sig.raster = raster.value;
    sig.color_control = color_control.value;
    const float census_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    std::memcpy(&sig.alpha_ref, &census_alpha_ref, 4);
    sig.clip = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
    sig.color_mask = color_mask;
    sig.viewport_w = uint32_t(std::fabs(viewport.width));
    sig.viewport_h = uint32_t(std::fabs(viewport.height));
    sig.scissor_w = uint32_t(scissor_rect.right - scissor_rect.left);
    sig.scissor_h = uint32_t(scissor_rect.bottom - scissor_rect.top);
    sig.prim = uint32_t(prim_type);
    sig.kind = uint32_t(pk.kind);
    sig.count_log2 = pk.count ? 32 - uint32_t(__builtin_clz(pk.count)) : 0;
    bool any_blend = false;
    for (uint32_t i = 0; i < color_count; ++i) any_blend = any_blend || blends[i].blendEnabled;
    sig.flags = (color_pass ? Census::kColorPass : 0) | (alpha_test ? Census::kAlphaTest : 0) |
                (alpha_to_mask ? Census::kAlphaToMask : 0) | (stencil_enabled ? Census::kStencil : 0) |
                (depth_enabled ? Census::kDepth : 0) | (depth_write ? Census::kDepthWrite : 0) |
                (any_blend ? Census::kBlend : 0) |
                ((raster.poly_offset_front_enable || raster.poly_offset_back_enable) ? Census::kBias : 0) |
                ((vs->entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT) ? Census::kMemExport : 0) |
                (defer ? Census::kDeferred : 0);
    pk.census_query = backend.census.Assign(sig);
  }
  if (defer) {
    if (!backend.recorder.deferred_.empty() && deferred_signature_ != window_signature) backend.recorder.FlushDeferred(1);
    deferred_signature_ = window_signature;
    pk.first_slot = uint32_t(deferred_slots_.size());
    deferred_slots_.insert(deferred_slots_.end(), draw_slots_.begin(), draw_slots_.end());
    backend.recorder.deferred_.push_back(pk);
    ++backend.stats_.deferred_draws;
    if (backend.recorder.deferred_.size() >= kMaxDeferred) backend.recorder.FlushDeferred(2);
    return true;
  }
  backend.recorder.FlushDeferred(3);
  backend.recorder.Submit(pk, draw_slots_.data(), uint32_t(draw_slots_.size()));
  return true;
}

}  // namespace renut::gpu
