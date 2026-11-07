// Resolves and depth copies.
#include "gpu/core/internal.h"

namespace renut::gpu {

// Direct3D leaves the copy in the device in the Xenos formats (the destination's format and
// endian, the clear values): the decoding that follows is all of the Xenos the host needs of a
// resolve, which names its texture and its rectangle by D3D objects. `w` is a kOpResolve record.
bool Resolve::DecodeResolve(const uint32_t* w, ResolveCopy& out) {
  out = ResolveCopy{};
  bits::RB_COPY_CONTROL control;
  control.value = w[5];
  if (control.copy_command != xenos::CopyCommand::kRaw && control.copy_command != xenos::CopyCommand::kConvert) {
    REXLOG_INFO("Renut: Plume resolve with copy command {} dropped", uint32_t(control.copy_command));
    return false;
  }
  bits::RB_COPY_DEST_INFO dest_info;
  dest_info.value = w[6];
  const bool is_depth = control.copy_src_select >= xenos::kMaxColorRenderTargets;
  // A depth copy is asked for in k_8_8_8_8 by Direct3D 9; the host copies the depth in the depth
  // surface's own format.
  // (the depth surface's format is bit 16 of its info word: 0 D24S8, 1 D24FS8)
  const auto depth_format = xenos::DepthRenderTargetFormat((backend.targets.native_surfaces_[4].info >> 16) & 1);
  out.dest_format = uint32_t(is_depth ? rex::graphics::DepthRenderTargetToTextureFormat(depth_format)
                                      : xenos::TextureFormat(dest_info.copy_dest_format));
  out.depth = is_depth;
  out.clear_depth = control.depth_clear_enable != 0;
  out.clear_color = !is_depth && control.color_clear_enable != 0;
  out.endian = uint32_t(dest_info.copy_dest_endian);
  out.swap = !is_depth && dest_info.copy_dest_swap != 0;
  out.depth_clear = w[7];
  out.color_clear = w[8];
  out.color_clear_lo = w[9];
  return true;
}

// A resolve record: the copy words Direct3D worked out, and the rectangle its copy draw would have
// covered.
void Resolve::NativeResolve(const uint32_t* w) {
  backend.recorder.FlushDeferred();
  ResolveCopy copy;
  if (!DecodeResolve(w, copy)) {
    ++backend.lists.native_bad_;
    return;
  }
  const uint32_t slot = std::min(w[10], 4u);
  ResolveSurfaces surfaces;
  surfaces.source = backend.targets.SurfaceTarget(slot);
  surfaces.depth = backend.targets.SurfaceTarget(4);
  surfaces.slot = slot;
  for (uint32_t i = 0; i < native_d3d::kResolveTraceWords; ++i) surfaces.trace[i] = w[17 + i];
  surfaces.x = w[1];
  surfaces.y = w[2];
  surfaces.width = w[3] > w[1] ? w[3] - w[1] : 0;
  surfaces.height = w[4] > w[2] ? w[4] - w[2] : 0;
  surfaces.object_base = w[11];
  surfaces.object_width = w[12];
  surfaces.object_height = w[13];
  surfaces.dest_dx = int32_t(w[14]);
  surfaces.dest_dy = int32_t(w[15]);
  surfaces.object_pitch = w[16];
  ++(surfaces.object_base ? backend.targets.object_resolves_ : backend.targets.volume_resolves_);
  ExecuteResolve(copy, surfaces);
}

bool Resolve::ExecuteResolve(const ResolveCopy& copy, const ResolveSurfaces& surfaces) {
  ++backend.stats_.resolves;
  // plume-skip bit 33554432 drops the resolve entirely. Attributing intervals
  // between timestamps proved unreliable on this tiler - the same frame read as
  // 83% resolve or 89% between-passes depending only on which side of the
  // pass-closing barrier the mark sat. Removing a category of work and
  // measuring the difference does not depend on that.
  if (backend.skip_mask_ & 33554432) return true;
  if (!surfaces.width || !surfaces.height) return true;
  const bool copying_depth = copy.depth;
  // The source is the surface the resolve names, the rectangle in its pixels:
  // the host's (a shadow map is drawn smaller than the guest thinks).
  Target* source = surfaces.source;
  // A surface whose image went to an earlier resolve and is read again: what it
  // held comes back first (AliasDepthResolve).
  if (source && source->restore_key) backend.targets.RestoreAliasedDepth(*source, false);
  const uint32_t x = HostFloor(source, surfaces.x), y = HostFloor(source, surfaces.y);
  const uint32_t width = HostCeil(source, surfaces.x + surfaces.width) - x;
  const uint32_t height = HostCeil(source, surfaces.y + surfaces.height) - y;
  if (!width || !height) return true;
  // plume-skip bit 134217728 drops every clear, these and Direct3D's. The
  // picture is wrong; it measures what the clear-only passes cost.
  const bool skip_clears = backend.skip_mask_ & 134217728;
  const uint32_t copy_height = height;
  backend.draw.Trace("resolve {} src=({},{}) {}x{} found={} -> {:08X} dfmt={} clear={}{}",
        copying_depth ? "depth" : "color", x, y, width, height,
        source != nullptr, surfaces.object_base, copy.dest_format, copy.clear_color ? "c" : "", copy.clear_depth ? "d" : "");
  // A volume's slices interleave in memory and have no texture object of their own to fill: the
  // copy is not made (the clears below still are). Counted by the surface report.
  if (!surfaces.object_base && volume_logged_ < 4) {
    ++volume_logged_;
    REXLOG_INFO("Renut: Plume resolve into a volume's slice not copied ({}x{})", width, height);
  }
  if (source && surfaces.object_base && x < source->width && y < source->height) {
    uint32_t sx = x, sy = y;
    uint32_t w = std::min(width, source->width - x), h = std::min(copy_height, source->height - y);
    const auto* dest_info = rex::graphics::FormatInfo::Get(xenos::TextureFormat(copy.dest_format));
    const uint32_t dest_bpp = dest_info ? std::max(1u, dest_info->bits_per_pixel / 8) : 4;
    // Depth resolves become R32_FLOAT textures holding the depth, which is what
    // a k_24_8 / k_24_8_FLOAT fetch returns in x (shadow maps, and the scene
    // depth the depth-of-field and fog passes read). The scene's (k_24_8_FLOAT)
    // also keep the stencil in y: the rain ripples read its water bit through
    // an 8_8_8_8 fetch of the same memory.
    const bool with_stencil =
        copying_depth && copy.dest_format == uint32_t(xenos::TextureFormat::k_24_8_FLOAT);
    const RenderFormat dest_format =
        copying_depth ? (with_stencil ? RenderFormat::R32G32_FLOAT : RenderFormat::R32_FLOAT) : source->format;
    // The copy goes into the D3DTexture the resolve names - the level, slice
    // or face Direct3D's own lookup found - at its own size, the source corner
    // landing on the point Direct3D was given; sampled through its fetch
    // constant.
    Resolved* chosen = nullptr;
    Resolved* aliased = nullptr;
    uint32_t dest_x = 0, dest_y = 0;
    {
      const int32_t scale = int32_t(source->scale);
      int32_t ox = int32_t(sx) + surfaces.dest_dx * scale / 4, oy = int32_t(sy) + surfaces.dest_dy * scale / 4;
      if (ox < 0) {
        const uint32_t cut = std::min(w, uint32_t(-ox));
        sx += cut, w -= cut, ox = 0;
      }
      if (oy < 0) {
        const uint32_t cut = std::min(h, uint32_t(-oy));
        sy += cut, h -= cut, oy = 0;
      }
      const uint32_t object_w = HostCeil(source, std::max(surfaces.object_width, 1u));
      const uint32_t object_h = HostCeil(source, std::max(surfaces.object_height, 1u));
      // A depth map taken whole into a texture of exactly its size (the
      // shadow maps) is not copied: see AliasDepthResolve. Anything else - the
      // scene's depth with its stencil, a part of a surface - copies as before.
      if (backend.swap.depth_alias_ && copying_depth && !with_stencil && source->depth && source->texture &&
          source->samples == 1 &&
          !(backend.skip_mask_ & (268435456 | 536870912)) && sx == 0 && sy == 0 && ox == 0 && oy == 0 &&
          w == source->width && h == source->height && object_w == w && object_h == h) {
        aliased = backend.targets.AliasDepthResolve(*source, surfaces.object_base);
        chosen = aliased;
      }
      if (!chosen) chosen = &backend.targets.GetResolved(surfaces.object_base, dest_format, object_w, object_h);
      dest_x = uint32_t(ox);
      dest_y = uint32_t(oy);
      w = dest_x < chosen->width ? std::min(w, chosen->width - dest_x) : 0;
      h = dest_y < chosen->height ? std::min(h, chosen->height - dest_y) : 0;
    }
    Resolved& dest = *chosen;
    // What the last resolve left here was never sampled: a copy nobody read (or one the host could have
    // skipped). Not counted for the first resolve into a texture.
    const bool unread = dest.written != 0 && !dest.sampled;
    // The same pixels again: nothing was drawn or cleared into the source since the last resolve into this
    // texture from it, the rectangle and the place in the texture are the same, and so is the kind.
    const bool redundant = dest.written != 0 && !aliased && dest.last_src == source &&
                           dest.last_src_surface == source->surface && dest.last_src_writes == source->writes &&
                           dest.last_rect[0] == sx && dest.last_rect[1] == sy && dest.last_rect[2] == w &&
                           dest.last_rect[3] == h && dest.last_dest[0] == dest_x && dest.last_dest[1] == dest_y &&
                           dest.last_depth == copying_depth;
    if (redundant) ++backend.stats_.resolves_redundant;
    dest.last_src = source;
    dest.last_src_surface = source->surface;
    dest.last_src_writes = source->writes;
    dest.last_rect[0] = sx, dest.last_rect[1] = sy, dest.last_rect[2] = w, dest.last_rect[3] = h;
    dest.last_dest[0] = dest_x, dest.last_dest[1] = dest_y;
    dest.last_depth = copying_depth;
    if (unread) ++backend.stats_.resolves_unread;
    // What became of the copy this one replaces (see Resolved::last_copy).
    if (dest.written != 0 && dest.last_copy) {
      const uint32_t kind = !dest.sampled ? 0u : dest.read_dirty ? 2u : 1u;
      ++backend.stats_.resolve_class[kind];
      backend.stats_.resolve_class_px[kind] += dest.last_px;
      if (backend.swap.resolve_log_) {
        static const char* const names[3] = {"unread", "alias_ok", "needs_copy"};
        LogResolveLine(fmt::format("f={} class {} {}px of the copy at {:08X} made by trace={:X},{:X},{:X}", backend.swaps_,
                                   names[kind], dest.last_px, surfaces.object_base, dest.last_trace[0],
                                   dest.last_trace[1], dest.last_trace[2]));
      }
    }
    dest.sampled = false;
    ++dest.generation;
    dest.scale = source->scale;
    dest.depth = copying_depth;
    dest.written = ++resolve_serial_;
    backend.targets.InvalidateResolvedAddress(surfaces.object_base);
    dest.written_frame = backend.swaps_;
    dest.guest_pitch = std::max(surfaces.object_pitch, 32u);
    dest.guest_height = (std::max(surfaces.object_height, 1u) + 31) & ~31u;
    dest.guest_bpp = dest_bpp;
    // The guest memory under it, as Direct3D has left it (FindResolved).
    {
      const uint32_t base_address = surfaces.object_base;
      dest.memory_epoch = native_d3d::TextureMemoryEpoch(base_address);
      dest.checked_frame = backend.swaps_;
    }
    if (aliased) {
      // Nothing to copy: the texture is the surface's own image.
      ++backend.stats_.resolves_aliased;
    } else if (dest.texture && copying_depth && w && h) {
      ++backend.stats_.resolves_depth;
      backend.stats_.resolve_px_depth += uint64_t(w) * h;
      const bool packed = with_stencil && dest.format == dest_format;
      const bool multisampled = source->samples > 1;
      // The scene's depth with its stencil: one compute pass into the texture, when the device allows it; the
      // staging buffers otherwise. A multisampled image cannot go through them (a buffer copy takes one sample
      // per texel): only the compute pass, which reads its first sample, resolves it.
      if (!(packed && (backend.swap.depth_direct_ || multisampled) && dest.format == RenderFormat::R32G32_FLOAT &&
            ResolveDepthStencil(*source, sx, sy, w, h, dest.texture.get(), dest.layout, dest_x, dest_y))) {
        if (multisampled) {
          if (++backend.stats_.msaa_depth_skipped <= 4) {
            REXLOG_WARN("Renut: Plume multisampled depth resolve not made (stencil {}, pipeline {})", packed,
                        backend.patches.dres_ms_pipeline_ != VK_NULL_HANDLE);
          }
        } else {
          backend.targets.Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
          CopyDepth(*source, sx, sy, w, h, dest.texture.get(), dest_x, dest_y, packed);
        }
      } else if (multisampled) {
        ++backend.stats_.msaa_depth_resolves;
      }
    } else if (dest.texture && w && h) {
      ++backend.stats_.resolves_color;
      backend.stats_.resolve_px_color += uint64_t(w) * h;
      backend.targets.Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
      backend.targets.Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      // plume-skip bit 268435456 keeps the resolve's barriers and pass breaks
      // but drops the copies themselves, colour and depth (see CopyDepth).
      if (!(backend.skip_mask_ & 268435456)) {
        if (source->samples > 1) {
          // The samples averaged into the texture (what the console's resolve does with the copy sample select
          // the game leaves: a colour resolve takes them all).
          const RenderRect region(int32_t(sx), int32_t(sy), int32_t(sx + w), int32_t(sy + h));
          backend.recorder.L()->resolveTextureRegion(dest.texture.get(), dest_x, dest_y, source->texture.get(), &region,
                                                     RenderResolveMode::AVERAGE);
          ++backend.stats_.msaa_color_resolves;
        } else {
          RenderBox box(int32_t(sx), int32_t(sy), int32_t(sx + w), int32_t(sy + h), 0, 1);
          backend.recorder.L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
                                   RenderTextureCopyLocation::Subresource(source->texture.get()), dest_x,
                                   dest_y, 0, &box);
        }
      }
    }
    dest.last_copy = !aliased && dest.texture && w && h;
    dest.read_dirty = false;
    dest.last_px = uint64_t(w) * h;
    for (uint32_t i = 0; i < 3; ++i) dest.last_trace[i] = surfaces.trace[2 + i];
    if (dest.texture) {
      if (backend.swap.snap_address_ && !dest.alias && surfaces.object_base == backend.swap.snap_address_ && ++backend.swap.snap_seen_ == backend.swap.snap_ordinal_) {
        SnapshotResolved(dest);
      }
      backend.targets.Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
      if (dest.descriptor) {
        backend.targets.SetTexture2D(dest.descriptor, dest.texture.get());
      }
      const uint32_t base_address = surfaces.object_base;
      if (backend.swap.resolve_log_) {
        // One line per resolve: what it is, where from (the target and its scale), where to, what rode
        // along, and whether the last one into the same texture was ever read.
        LogResolveLine(fmt::format(
            "f={} resolve {} {}x{} at ({},{}) from slot {} target {}x{} s{} -> {:08X} obj={}x{} pitch={} dfmt={} "
            "endian={} swap={} host_fmt={} how={} clear={}{} prev_unread={} redundant={} trace={:X},{:X},{:X},{:X},{:X},{:X},{:X},{:X}",
            backend.swaps_, copying_depth ? "depth" : "color", w, h, dest_x, dest_y, surfaces.slot,
            source->width, source->height, source->scale, base_address, surfaces.object_width,
            surfaces.object_height, dest.guest_pitch, copy.dest_format, copy.endian, uint32_t(copy.swap),
            uint32_t(dest.format), aliased ? "alias" : (dest.texture && w && h ? "copy" : "none"),
            copy.clear_color ? "c" : "", copy.clear_depth ? "d" : "", unread ? 1 : 0, redundant ? 1 : 0,
            surfaces.trace[0], surfaces.trace[1], surfaces.trace[2], surfaces.trace[3], surfaces.trace[4],
            surfaces.trace[5], surfaces.trace[6], surfaces.trace[7]));
      }
    }
  }

  // Clears that ride along with the resolve.
  if (copy.clear_color) ++backend.stats_.resolve_clear_color;
  if (copy.clear_depth) ++backend.stats_.resolve_clear_depth;
  const RenderRect rect(int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height));
  if (copy.clear_color && source && !skip_clears) {
    backend.targets.Transition(source->texture.get(), source->layout, RenderTextureLayout::COLOR_WRITE);
    std::array<Target*, 4> colors{source};
    backend.targets.SetFramebuffer(backend.targets.GetFramebuffer(colors, 1, nullptr));
    RenderColor color;
    if (source->format == RenderFormat::R8G8B8A8_UNORM) {
      const uint32_t c = copy.color_clear;
      color = RenderColor(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f,
                          (c >> 24) / 255.f);
    } else {
      auto half = [](uint32_t h) {
        uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
        float v = exp ? std::ldexp(1.f + mant / 1024.f, int(exp) - 15) : std::ldexp(mant / 1024.f, -14);
        return sign ? -v : v;
      };
      color = RenderColor(half(copy.color_clear >> 16), half(copy.color_clear & 0xFFFF),
                          half(copy.color_clear_lo >> 16), half(copy.color_clear_lo & 0xFFFF));
    }
    backend.recorder.L()->clearColor(0, color, &rect, 1);
    ++backend.stats_.clears;
  }
  if (copy.clear_depth && !skip_clears) {
    Target* depth = surfaces.depth;
    if (depth) {
      const RenderRect depth_rect(int32_t(HostFloor(depth, surfaces.x)), int32_t(HostFloor(depth, surfaces.y)),
                                  int32_t(HostCeil(depth, surfaces.x + surfaces.width)),
                                  int32_t(HostCeil(depth, surfaces.y + surfaces.height)));
      // A surface whose image went to a resolve: a clear of all of it needs
      // nothing from the old one, a smaller rectangle needs it back.
      if (depth->restore_key || depth->aliased_since_clear || depth->depth_undefined) {
        const bool whole = depth_rect.left <= 0 && depth_rect.top <= 0 &&
                           depth_rect.right >= int32_t(depth->width) && depth_rect.bottom >= int32_t(depth->height);
        backend.targets.NoteAliasedUse(*depth, whole);
        if (depth->restore_key) backend.targets.RestoreAliasedDepth(*depth, whole);
        if (whole) depth->depth_undefined = false;  // this clear takes both aspects
      }
      backend.targets.Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      backend.targets.SetFramebuffer(backend.targets.GetFramebuffer(none, 0, depth));
      const uint32_t d = copy.depth_clear;
      backend.recorder.L()->clearDepthStencil(true, true, float(d >> 8) / float(0xFFFFFF), d & 0xFF, &depth_rect, 1);
      ++backend.stats_.clears;
    }
  }
  return true;
}

void Resolve::LogResolveLine(const std::string& line) {
  log_buffer_ += line;
  log_buffer_ += '\n';
}

void Resolve::FlushResolveLog() {
  if (log_buffer_.empty() || backend.miss_dump_directory_.empty()) {
    log_buffer_.clear();
    return;
  }
  const std::string root = backend.miss_dump_directory_.substr(0, backend.miss_dump_directory_.rfind('/'));
  if (FILE* file = std::fopen((root + "/plume-resolve.log").c_str(), "a")) {
    std::fwrite(log_buffer_.data(), 1, log_buffer_.size(), file);
    std::fclose(file);
  }
  log_buffer_.clear();
}

// Photo Mode's picture: render target 0, after the draws recorded before the capture, read back
// and written into the photo texture's rows (width x height texels of guest pixels, `pitch` bytes
// apart) in the order the CPU keeps a linear X8R8G8B8 texel: alpha, red, green, blue. The game thread
// waits for the list this record is in. A target drawn at a fraction of the guest's resolution is
// written at the guest's: each guest pixel takes the host texel under it.
void Resolve::NativePhoto(const uint32_t* w) {
  backend.recorder.FlushDeferred();
  const uint32_t address = w[1], pitch = w[2], width = w[3], height = w[4];
  Target* source = backend.targets.SurfaceTarget(0);
  if (!source || !source->texture || !source->scale || !address || !width || !height) {
    ++backend.lists.native_bad_;
    return;
  }
  const bool rgba8 = source->format == RenderFormat::R8G8B8A8_UNORM;
  const bool half = source->format == RenderFormat::R16G16B16A16_FLOAT;
  if (!rgba8 && !half) {
    REXLOG_INFO("Renut: Plume photo capture skipped: host format {}", uint32_t(source->format));
    return;
  }
  const uint32_t scale = source->scale;
  const uint32_t host_w = std::min(source->width, HostCeil(source, width));
  const uint32_t host_h = std::min(source->height, HostCeil(source, height));
  if (!host_w || !host_h) return;
  const uint32_t texel = rgba8 ? 4 : 8;
  const uint32_t row_texels = (host_w + 63) & ~63u;
  auto buffer = backend.device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(row_texels) * host_h * texel));
  if (!buffer) return;
  backend.targets.Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
  RenderBox box(0, 0, int32_t(host_w), int32_t(host_h), 0, 1);
  RenderTexture* readable = source->texture.get();
  // A multisampled target cannot be copied to a buffer: its samples are averaged into a single-sample image first
  // (kept until the flush below has run).
  std::unique_ptr<RenderTexture> averaged;
  RenderTextureLayout averaged_layout = RenderTextureLayout::UNKNOWN;
  if (source->samples > 1) {
    averaged = backend.device_->createTexture(RenderTextureDesc::Texture2D(host_w, host_h, 1, source->format));
    if (!averaged) return;
    backend.targets.Transition(averaged.get(), averaged_layout, RenderTextureLayout::COPY_DEST);
    const RenderRect region(0, 0, int32_t(host_w), int32_t(host_h));
    backend.recorder.L()->resolveTextureRegion(averaged.get(), 0, 0, source->texture.get(), &region,
                                               RenderResolveMode::AVERAGE);
    backend.targets.Transition(averaged.get(), averaged_layout, RenderTextureLayout::COPY_SOURCE);
    readable = averaged.get();
  }
  CopyTextureToBuffer(backend.recorder.L(), buffer.get(), readable, host_w, host_h, row_texels, 0, 0, &box);
  backend.targets.Flush();
  const auto* pixels = static_cast<const uint8_t*>(buffer->map());
  if (!pixels) return;
  uint8_t* guest = backend.memory_->TranslatePhysical<uint8_t*>(address);
  auto to8 = [](uint16_t bits) -> uint8_t {
    const uint32_t exp = (bits >> 10) & 0x1F, mant = bits & 0x3FF;
    if (bits & 0x8000) return 0;
    float v = exp ? std::ldexp(1.f + mant / 1024.f, int(exp) - 15) : std::ldexp(mant / 1024.f, -14);
    if (exp == 31) v = 1.f;
    return uint8_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
  };
  const uint32_t guest_w = std::min(width, uint32_t(uint64_t(host_w) * 4 / scale));
  const uint32_t guest_h = std::min(height, uint32_t(uint64_t(host_h) * 4 / scale));
  for (uint32_t gy = 0; gy < guest_h; ++gy) {
    const uint32_t y = std::min(uint32_t(uint64_t(gy) * scale / 4), host_h - 1);
    const uint8_t* src = pixels + size_t(y) * row_texels * texel;
    uint8_t* out = guest + size_t(gy) * pitch;
    for (uint32_t gx = 0; gx < guest_w; ++gx) {
      const uint32_t x = std::min(uint32_t(uint64_t(gx) * scale / 4), host_w - 1);
      uint8_t r, g, b;
      if (rgba8) {
        r = src[x * 4], g = src[x * 4 + 1], b = src[x * 4 + 2];
      } else {
        const auto* t = reinterpret_cast<const uint16_t*>(src + x * 8);
        r = to8(t[0]), g = to8(t[1]), b = to8(t[2]);
      }
      out[gx * 4 + 0] = 0xFF;
      out[gx * 4 + 1] = r;
      out[gx * 4 + 2] = g;
      out[gx * 4 + 3] = b;
    }
  }
  buffer->unmap();
  ++photo_captures_;
  REXLOG_INFO("Renut: Plume photo capture {}: {}x{} guest texels into {:08X}, pitch {}, from a {}x{} {} target at scale {}",
              photo_captures_, guest_w, guest_h, address, pitch, host_w, host_h, rgba8 ? "8888" : "16f", scale);
}

bool Resolve::CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                      RenderTexture* dest, uint32_t dest_x, uint32_t dest_y, bool with_stencil) {
  // Vulkan copies a depth aspect only to a buffer, so go through one: depth
  // to buffer (4 bytes per texel for D32_SFLOAT), buffer to the colour image.
  // With the stencil, the S8 aspect goes to a second buffer and a compute pass
  // joins the two into RG32F texels (depth_stencil_pack.comp).
  constexpr uint64_t kStagingSize = 4096ull * 2048 * 4;
  if (uint64_t(w) * h * 4 > kStagingSize) return false;
  if (!depth_staging_) {
    depth_staging_ = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        kStagingSize, RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!depth_staging_) return false;
  }
  if (with_stencil && !stencil_staging_) {
    const auto flags = RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE;
    stencil_staging_ = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize / 4, flags));
    packed_staging_ = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize * 2, flags));
    if (!stencil_staging_ || !packed_staging_ || !backend.patches.CreatePackPipeline()) {
      REXLOG_WARN("Renut: Plume cannot keep the stencil of depth resolves");
      stencil_staging_.reset();
      packed_staging_.reset();
    }
  }
  with_stencil = with_stencil && packed_staging_ && backend.patches.pack_pipeline_ != VK_NULL_HANDLE;
  // Always a barrier call, which also ends any open render pass before the
  // raw Vulkan commands below.
  backend.recorder.L()->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(source.texture.get(), RenderTextureLayout::COPY_SOURCE));
  ++backend.stats_.barriers;
  source.layout = RenderTextureLayout::COPY_SOURCE;
  backend.targets.bound_framebuffer_ = nullptr;
  // plume-skip bit 536870912 keeps that barrier and drops the round trip
  // through the staging buffer, to price it; 268435456 drops every copy.
  if (backend.skip_mask_ & (268435456 | 536870912)) return true;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(backend.recorder.L())->vk;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {int32_t(x), int32_t(y), 0};
  region.imageExtent = {w, h, 1};
  VkImage image = static_cast<VulkanTexture*>(source.texture.get())->vk;
  vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<VulkanBuffer*>(depth_staging_.get())->vk, 1, &region);
  if (with_stencil) {
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(stencil_staging_.get())->vk, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    struct {
      uint64_t depth, stencil, packed;
      uint32_t count, pad;
    } push = {depth_staging_->getDeviceAddress(), stencil_staging_->getDeviceAddress(),
              packed_staging_->getDeviceAddress(), w * h, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, backend.patches.pack_pipeline_);
    vkCmdPushConstants(cb, backend.pack_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (w * h + 63) / 64, 1, 1);
    // The compute layout's push constants may disturb the graphics ones.
    backend.targets.pushed_constants_[0] = backend.targets.pushed_constants_[1] = backend.targets.pushed_constants_[2] = UINT64_MAX;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    backend.recorder.L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                             RenderTextureCopyLocation::PlacedFootprint(packed_staging_.get(),
                                                                        RenderFormat::R32G32_FLOAT, w, h, 1, w),
                             dest_x, dest_y);
    return true;
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  backend.recorder.L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                           RenderTextureCopyLocation::PlacedFootprint(depth_staging_.get(),
                                                                      RenderFormat::R32_FLOAT, w, h, 1, w),
                           dest_x, dest_y);
  return true;
}

bool Resolve::ResolveDepthStencil(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RenderTexture* dest,
                                  RenderTextureLayout& dest_layout, uint32_t dest_x, uint32_t dest_y) {
  // The debug skips of CopyDepth keep their meaning there.
  if (backend.skip_mask_ & (268435456 | 536870912)) return false;
  Patches& patches = backend.patches;
  if (!patches.CreateDepthResolvePipeline()) return false;
  const VkPipeline pipeline = source.samples > 1 ? patches.dres_ms_pipeline_ : patches.dres_pipeline_;
  if (pipeline == VK_NULL_HANDLE) return false;
  auto* src = static_cast<VulkanTexture*>(source.texture.get());
  auto* dst = static_cast<VulkanTexture*>(dest);
  const uint32_t slot = backend.slot_;
  if (!src || !dst || src->imageView == VK_NULL_HANDLE || dst->imageView == VK_NULL_HANDLE ||
      patches.dres_used_[slot] >= Patches::kDresSets || !w || !h) {
    return false;
  }
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  // The stencil aspect needs a view of its own (Plume's is the depth one); it and the set live until the slot's
  // frame has finished.
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = src->vk;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = src->imageFormat;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  VkImageView stencil_view = VK_NULL_HANDLE;
  if (vkCreateImageView(vk_device, &view_info, nullptr, &stencil_view) != VK_SUCCESS) return false;
  patches.dres_views_[slot].push_back(stencil_view);
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = patches.dres_pool_[slot];
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &patches.dres_set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(vk_device, &alloc, &set) != VK_SUCCESS) return false;
  ++patches.dres_used_[slot];
  VkDescriptorImageInfo images[3] = {};
  images[0] = {VK_NULL_HANDLE, src->imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[1] = {VK_NULL_HANDLE, stencil_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  images[2] = {VK_NULL_HANDLE, dst->imageView, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(vk_device, 3, writes, 0, nullptr);
  // The barriers end any open render pass, and name the compute stage so that the dispatch waits for them
  // (Targets::Transition's are for the graphics ones).
  backend.recorder.L()->barriers(RenderBarrierStage::COMPUTE,
                                 RenderTextureBarrier(source.texture.get(), RenderTextureLayout::SHADER_READ));
  backend.recorder.L()->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(dest, RenderTextureLayout::GENERAL));
  backend.stats_.barriers += 2;
  source.layout = RenderTextureLayout::SHADER_READ;
  dest_layout = RenderTextureLayout::GENERAL;
  backend.targets.bound_framebuffer_ = nullptr;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(backend.recorder.L())->vk;
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, patches.dres_layout_, 0, 1, &set, 0, nullptr);
  const int32_t push[6] = {int32_t(x), int32_t(y), int32_t(dest_x), int32_t(dest_y), int32_t(w), int32_t(h)};
  vkCmdPushConstants(cb, patches.dres_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
  // The compute layout's push constants may disturb the graphics ones.
  backend.targets.pushed_constants_[0] = backend.targets.pushed_constants_[1] = backend.targets.pushed_constants_[2] = UINT64_MAX;
  ++backend.stats_.depth_direct;
  return true;
}

void Resolve::SnapshotResolved(Resolved& dest) {
  if (!snap_ || backend.swap.snap_width_ != dest.width || backend.swap.snap_height_ != dest.height || backend.swap.snap_format_ != dest.format) {
    snap_ = backend.device_->createTexture(RenderTextureDesc::Texture2D(dest.width, dest.height, 1, dest.format));
    backend.swap.snap_width_ = dest.width;
    backend.swap.snap_height_ = dest.height;
    backend.swap.snap_format_ = dest.format;
    snap_layout_ = RenderTextureLayout::UNKNOWN;
  }
  if (snap_) {
    backend.targets.Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_SOURCE);
    backend.targets.Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_DEST);
    backend.recorder.L()->copyTexture(snap_.get(), dest.texture.get());
    backend.swap.snap_taken_ = true;
  }
}


// Puts every image a frame wrote into one known layout before the frame ends.
//
// Two frames in flight share these textures, and Transition only emits a
// barrier when the layout it tracks changes - so without this the next frame
// would write a target the last frame wrote with no image barrier in between.
// An execution barrier is not enough: these are UBWC-compressed on an Adreno,
// and writing one from two submissions with no layout transition between them
// leaves the compression metadata inconsistent, which shows up as a blocky,
// darkened frame. Landing on SHADER_READ here guarantees the next frame's first
// use transitions back, and a layout transition preserves the contents.
void Resolve::FlushImagesForNextFrame() {
  for (auto& [key, target] : backend.targets.targets_) {
    if (target.texture) backend.targets.Transition(target.texture.get(), target.layout, RenderTextureLayout::SHADER_READ);
  }
  for (auto& [key, r] : backend.targets.resolved_) {
    if (r.texture) backend.targets.Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
    if (r.fitted) backend.targets.Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::SHADER_READ);
  }
}

void Resolve::LogSkinned() {
  std::lock_guard<std::mutex> lock(backend.swap.skinned_mutex_);
  REXLOG_ERROR("Renut: Plume skinned vertex shaders seen: {}", skinned_seen_.size());
  for (const auto& [hash, seen] : skinned_seen_) {
    REXLOG_ERROR("Renut: Plume   skinned vs={:016X} draws={} frames {}..{}", hash, seen.draws, seen.first_frame,
                 seen.last_frame);
  }
}

void Resolve::ValidateRange(const char* what, uint64_t draw_hash, RenderBuffer* buffer, uint64_t offset,
                                          uint32_t guest_address, uint32_t size, xenos::Endian endian) {
  if (!buffer || buffer == backend.draw.upload_.get() || !size) return;
  std::vector<uint8_t> guest(size);
  CopySwapped(guest.data(), backend.memory_->TranslatePhysical<const uint8_t*>(guest_address), size, endian);
  const auto* window = static_cast<const uint8_t*>(buffer->map());
  if (!window) return;
  uint32_t differ = 0, first = UINT32_MAX;
  for (uint32_t i = 0; i < size; ++i) {
    if (window[offset + i] != guest[i]) {
      if (first == UINT32_MAX) first = i;
      ++differ;
    }
  }
  buffer->unmap();
  ++validate_ranges_;
  if (!differ) return;
  // A write that landed since the window was made has already been recorded (the callback runs before
  // the write completes); the next drain drops the window. Only a difference with nothing pending counts.
  const uint64_t drops_before = kept_drop_events_;
  backend.streams.DrainWatchHits();
  if (kept_drop_events_ != drops_before) return;
  ++validate_stale_;
  if (validate_logged_ < 120) {
    ++validate_logged_;
    REXLOG_ERROR("Renut: Plume STALE {} at draw: frame {} vs/ps hash {:016X} guest {:08X}+{:X}: {} bytes differ, first at +{:X}",
                 what, backend.swaps_, draw_hash, guest_address, size, differ, first);
  }
}

}  // namespace renut::gpu
