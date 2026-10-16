// Resolves and depth copies.
#include "internal.h"

namespace renut::gpu {

bool PlumeCommandProcessor::ResolveInfoForRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                               draw_util::ResolveInfo& info) {
  if (!resolve_regs_) resolve_regs_ = std::make_unique<RegisterFile>();
  if (!resolve_vertices_) {
    // Guest physical memory the SDK reads the vertices from.
    resolve_vertices_ = rex::system::kernel_memory()->SystemHeapAlloc(64, 16, rex::memory::kSystemHeapPhysical);
    if (!resolve_vertices_) return false;
  }
  const uint32_t physical = memory_->GetPhysicalAddress(resolve_vertices_);
  if (physical == UINT32_MAX) return false;
  const float vertices[6] = {float(x0), float(y0), float(x1), float(y0), float(x0), float(y1)};
  std::memcpy(memory_->TranslatePhysical<uint8_t*>(physical), vertices, sizeof(vertices));

  RegisterFile& regs = *resolve_regs_;
  std::memcpy(regs.values, register_file_->values, sizeof(regs.values));
  xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = physical >> 2;
  fetch.endian = xenos::Endian::kNone;
  fetch.size = 6;
  regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = fetch.dword_0;
  regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1] = fetch.dword_1;
  reg::PA_SU_VTX_CNTL vertex_control;
  vertex_control.value = regs.values[XE_GPU_REG_PA_SU_VTX_CNTL];
  vertex_control.pix_center = xenos::PixelCenter::kOGLHalf;  // no half-pixel offset to undo
  regs.values[XE_GPU_REG_PA_SU_VTX_CNTL] = vertex_control.value;
  reg::PA_SU_SC_MODE_CNTL mode_control;
  mode_control.value = regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  mode_control.vtx_window_offset_enable = 0;
  regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode_control.value;
  reg::PA_SC_WINDOW_SCISSOR_TL window_tl;
  window_tl.value = 0;
  window_tl.window_offset_disable = 1;
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = window_tl.value;
  reg::PA_SC_WINDOW_SCISSOR_BR window_br;
  window_br.value = 0;
  window_br.br_x = 8192;
  window_br.br_y = 8192;
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = window_br.value;
  reg::PA_SC_SCREEN_SCISSOR_TL screen_tl;
  screen_tl.value = 0;
  regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL] = screen_tl.value;
  reg::PA_SC_SCREEN_SCISSOR_BR screen_br;
  screen_br.value = 0;
  screen_br.br_x = 8192;
  screen_br.br_y = 8192;
  regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = screen_br.value;
  return draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info);
}

// A resolve record: the copy registers Direct3D worked out, and the
// rectangle its copy draw would have covered.
void PlumeCommandProcessor::NativeResolve(const uint32_t* w) {
  FlushDeferred();
  for (uint32_t i = 0; i < 12; ++i) WriteRegister(0x2318 + i, w[5 + i]);
  for (uint32_t i = 0; i < 6; ++i) WriteRegister(0x2000 + i, w[17 + i]);
  draw_util::ResolveInfo info;
  if (!ResolveInfoForRect(int32_t(w[1]), int32_t(w[2]), int32_t(w[3]), int32_t(w[4]), info)) {
    ++native_bad_;
    return;
  }
  // The source is the surface bound in the slot the resolve names; its
  // register is RB_COLOR_INFO, _DEPTH_INFO or RB_COLOR1..3_INFO.
  static constexpr uint32_t kRegisterOfSlot[5] = {1, 3, 4, 5, 2};
  // Bit 8 of the source word: Photo Mode's capture routine issued this resolve
  // (native_d3d.cpp), the one copied to the CPU. Not tied to an address: the
  // scratch texture it resolves into is built on the spot.
  capture_this_resolve_ = (w[23] & 0x100) != 0;
  const uint32_t slot = std::min(w[23] & 0xFFu, 4u);
  ResolveSurfaces surfaces;
  surfaces.source = NativeTarget(slot, w[17 + kRegisterOfSlot[slot]]);
  surfaces.depth = SurfaceTarget(4);
  surfaces.x = w[1];
  surfaces.y = w[2];
  surfaces.width = w[3] > w[1] ? w[3] - w[1] : 0;
  surfaces.height = w[4] > w[2] ? w[4] - w[2] : 0;
  surfaces.object_base = w[24];
  surfaces.object_width = w[25];
  surfaces.object_height = w[26];
  surfaces.dest_dx = int32_t(w[27]);
  surfaces.dest_dy = int32_t(w[28]);
  surfaces.object_pitch = w[29];
  ++(surfaces.object_base ? object_resolves_ : address_resolves_count_);
  ExecuteResolve(info, surfaces);
  capture_this_resolve_ = false;
}

bool PlumeCommandProcessor::ExecuteResolve(const draw_util::ResolveInfo& info, const ResolveSurfaces& surfaces) {
  ++stats_.resolves;
  // plume-skip bit 33554432 drops the resolve entirely. Attributing intervals
  // between timestamps proved unreliable on this tiler - the same frame read as
  // 83% resolve or 89% between-passes depending only on which side of the
  // pass-closing barrier the mark sat. Removing a category of work and
  // measuring the difference does not depend on that.
  if (skip_mask_ & 33554432) return true;
  if (!surfaces.width || !surfaces.height) return true;
  const bool copying_depth = info.IsCopyingDepth();
  // The source is the surface the resolve names, the rectangle in its pixels:
  // the host's (a shadow map is drawn smaller than the guest thinks).
  Target* source = surfaces.source;
  // A surface whose image went to an earlier resolve and is read again: what it
  // held comes back first (AliasDepthResolve).
  if (source && source->restore_key) RestoreAliasedDepth(*source, false);
  const uint32_t x = HostFloor(source, surfaces.x), y = HostFloor(source, surfaces.y);
  const uint32_t width = HostCeil(source, surfaces.x + surfaces.width) - x;
  const uint32_t height = HostCeil(source, surfaces.y + surfaces.height) - y;
  if (!width || !height) return true;
  // plume-skip bit 134217728 drops every clear, these and Direct3D's. The
  // picture is wrong; it measures what the clear-only passes cost.
  const bool skip_clears = skip_mask_ & 134217728;
  const uint32_t copy_height = height;
  Trace("resolve {} src=({},{}) {}x{} found={} -> {:08X} dfmt={} ({},{}) pitch={} h={} clear={}{}",
        copying_depth ? "depth" : "color", x, y, width, height,
        source != nullptr, uint32_t(info.copy_dest_base), uint32_t(info.copy_dest_info.copy_dest_format),
        info.copy_dest_coordinate_info.offset_x_div_8 * 8, info.copy_dest_coordinate_info.offset_y_div_8 * 8,
        info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
        info.copy_dest_coordinate_info.height_aligned_div_32 * 32, info.IsClearingColor() ? "c" : "",
        info.IsClearingDepth() ? "d" : "");
  if (source && x < source->width && y < source->height &&
      info.rb_copy_control.copy_command != xenos::CopyCommand::kNull) {
    uint32_t sx = x, sy = y;
    uint32_t w = std::min(width, source->width - x), h = std::min(copy_height, source->height - y);
    const uint32_t dest_w =
        std::max(HostCeil(source, uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32u), w);
    const uint32_t dest_h =
        std::max(HostCeil(source, uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32u), h);
    const uint32_t dx = HostFloor(source, info.copy_dest_coordinate_info.offset_x_div_8 * 8);
    const uint32_t dy = HostFloor(source, info.copy_dest_coordinate_info.offset_y_div_8 * 8);
    const auto* dest_info = rex::graphics::FormatInfo::Get(
        xenos::TextureFormat(uint32_t(info.copy_dest_info.copy_dest_format)));
    const uint32_t dest_bpp = dest_info ? std::max(1u, dest_info->bits_per_pixel / 8) : 4;
    // Depth resolves become R32_FLOAT textures holding the depth, which is what
    // a k_24_8 / k_24_8_FLOAT fetch returns in x (shadow maps, and the scene
    // depth the depth-of-field and fog passes read). The scene's (k_24_8_FLOAT)
    // also keep the stencil in y: the rain ripples read its water bit through
    // an 8_8_8_8 fetch of the same memory.
    const bool with_stencil =
        copying_depth && uint32_t(info.copy_dest_info.copy_dest_format) ==
                             uint32_t(xenos::TextureFormat::k_24_8_FLOAT);
    const RenderFormat dest_format =
        copying_depth ? (with_stencil ? RenderFormat::R32G32_FLOAT : RenderFormat::R32_FLOAT) : source->format;
    // The copy goes into the D3DTexture the resolve names - the level, slice
    // or face Direct3D's own lookup found - at its own size, the source corner
    // landing on the point Direct3D was given; sampled through its fetch
    // constant. A 3D texture's slices interleave: those go by the copy's
    // address and offset.
    const bool to_object = surfaces.object_base != 0;
    Resolved* chosen = nullptr;
    Resolved* aliased = nullptr;
    uint32_t dest_x = 0, dest_y = 0;
    if (to_object) {
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
      if (depth_alias_ && copying_depth && !with_stencil && source->depth && source->texture &&
          !(skip_mask_ & (268435456 | 536870912)) && sx == 0 && sy == 0 && ox == 0 && oy == 0 &&
          w == source->width && h == source->height && object_w == w && object_h == h) {
        aliased = AliasDepthResolve(*source, surfaces.object_base);
        chosen = aliased;
      }
      if (!chosen) chosen = &GetResolved(surfaces.object_base, dest_format, object_w, object_h);
      dest_x = uint32_t(ox);
      dest_y = uint32_t(oy);
      w = dest_x < chosen->width ? std::min(w, chosen->width - dest_x) : 0;
      h = dest_y < chosen->height ? std::min(h, chosen->height - dest_y) : 0;
    } else {
      chosen = &GetResolved(info.copy_dest_base, dest_format, dest_w + dx, dest_h + dy);
      dest_x = dx;
      dest_y = dy;
    }
    Resolved& dest = *chosen;
    ++dest.generation;
    dest.scale = source->scale;
    dest.depth = copying_depth;
    dest.written = ++resolve_serial_;
    InvalidateResolvedLookups();
    dest.written_frame = swaps_;
    dest.guest_pitch = to_object ? std::max(surfaces.object_pitch, 32u) : dest_w;
    dest.guest_height = to_object ? (std::max(surfaces.object_height, 1u) + 31) & ~31u : dest_h;
    dest.guest_bpp = dest_bpp;
    // Snapshot of the guest memory under the whole resolved texture.
    {
      const uint32_t base_address = to_object ? surfaces.object_base : info.copy_dest_base;
      dest.guest_bytes = dest.guest_pitch * dest.guest_height * dest_bpp;
      dest.guest_hash = ContentHash(memory_->TranslatePhysical<const uint8_t*>(base_address), dest.guest_bytes);
      dest.checked_frame = swaps_;
    }
    if (aliased) {
      // Nothing to copy: the texture is the surface's own image.
    } else if (dest.texture && copying_depth && w && h) {
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      CopyDepth(*source, sx, sy, w, h, dest.texture.get(), dest_x, dest_y, with_stencil && dest.format == dest_format);
    } else if (dest.texture && w && h) {
      Transition(source->texture.get(), source->layout, RenderTextureLayout::COPY_SOURCE);
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_DEST);
      // plume-skip bit 268435456 keeps the resolve's barriers and pass breaks
      // but drops the copies themselves, colour and depth (see CopyDepth).
      if (!(skip_mask_ & 268435456)) {
        RenderBox box(int32_t(sx), int32_t(sy), int32_t(sx + w), int32_t(sy + h), 0, 1);
        L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.texture.get()),
                                 RenderTextureCopyLocation::Subresource(source->texture.get()), dest_x,
                                 dest_y, 0, &box);
      }
    }
    if (dest.texture) {
      if (snap_address_ && !dest.alias && info.copy_dest_base == snap_address_ && ++snap_seen_ == snap_ordinal_) {
        SnapshotResolved(dest);
      }
      Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
      if (dest.descriptor) {
        SetTexture2D(dest.descriptor, dest.texture.get());
      }
      const uint32_t base_address = to_object ? surfaces.object_base : info.copy_dest_base;
      if (resolve_log_ && !copying_depth && w >= 64 && h >= 64) {
        LogResolveLine(fmt::format(
            "f={} resolve {}x{} at ({},{}) -> {:08X} {} obj={}x{} pitch={} dfmt={} endian={} swap={} host_fmt={}",
            swaps_, w, h, dest_x, dest_y, base_address, to_object ? "object" : "address", surfaces.object_width,
            surfaces.object_height, dest.guest_pitch, uint32_t(info.copy_dest_info.copy_dest_format),
            uint32_t(info.copy_dest_info.copy_dest_endian), uint32_t(info.copy_dest_info.copy_dest_swap),
            uint32_t(dest.format)));
      }
      // Only the resolve Photo Mode's capture routine issued (capture_this_resolve_): one copy
      // to the CPU per photo, and none while playing.
      if (capture_this_resolve_) {
        ++photo_captures_;
        REXLOG_INFO("Renut: Plume photo capture {}: {}x{} at ({},{}) -> {:08X} {} pitch {} bpp {} format {} depth {}",
                    photo_captures_, w, h, dest_x, dest_y, base_address, to_object ? "object" : "address",
                    dest.guest_pitch, dest_bpp, uint32_t(dest.format), copying_depth);
        if (!copying_depth && w && h && base_address) {
          ReadbackResolve(dest, info, base_address, dest_x, dest_y, w, h, dest.guest_pitch, dest_bpp, source->scale);
        }
      }
    }
  }

  // Clears that ride along with the resolve.
  const RenderRect rect(int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height));
  if (info.IsClearingColor() && source && !skip_clears) {
    Transition(source->texture.get(), source->layout, RenderTextureLayout::COLOR_WRITE);
    std::array<Target*, 4> colors{source};
    SetFramebuffer(GetFramebuffer(colors, 1, nullptr));
    RenderColor color;
    if (source->format == RenderFormat::R8G8B8A8_UNORM) {
      const uint32_t c = info.rb_color_clear;
      color = RenderColor(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f,
                          (c >> 24) / 255.f);
    } else {
      auto half = [](uint32_t h) {
        uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
        float v = exp ? std::ldexp(1.f + mant / 1024.f, int(exp) - 15) : std::ldexp(mant / 1024.f, -14);
        return sign ? -v : v;
      };
      color = RenderColor(half(info.rb_color_clear >> 16), half(info.rb_color_clear & 0xFFFF),
                          half(info.rb_color_clear_lo >> 16), half(info.rb_color_clear_lo & 0xFFFF));
    }
    L()->clearColor(0, color, &rect, 1);
    ++stats_.clears;
  }
  if (info.IsClearingDepth() && !skip_clears) {
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
        NoteAliasedUse(*depth, whole);
        if (depth->restore_key) RestoreAliasedDepth(*depth, whole);
        if (whole) depth->depth_undefined = false;  // this clear takes both aspects
      }
      Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      std::array<Target*, 4> none{};
      SetFramebuffer(GetFramebuffer(none, 0, depth));
      const uint32_t d = info.rb_depth_clear;
      L()->clearDepthStencil(true, true, float(d >> 8) / float(0xFFFFFF), d & 0xFF, &depth_rect, 1);
      ++stats_.clears;
    }
  }
  return true;
}

void PlumeCommandProcessor::LogResolveLine(const std::string& line) {
  if (miss_dump_directory_.empty()) return;
  const std::string root = miss_dump_directory_.substr(0, miss_dump_directory_.rfind('/'));
  if (FILE* file = std::fopen((root + "/plume-resolve.log").c_str(), "a")) {
    std::fputs(line.c_str(), file);
    std::fputc('\n', file);
    std::fclose(file);
  }
}

// The CPU reads a few resolves directly: Photo Mode copies the screen into a texture,
// waits for the copy and encodes a JPEG (and the 90x50 thumbnail) from the texels. Plume
// keeps resolves on the GPU, so that read saw whatever the memory held - black. This runs
// the frame so far, waits for it, and writes the pixels into guest memory the way the
// hardware would have: 32x32 tiled, the resolve's endianness and red/blue order.
// The fence the game waits on is recorded in the same list after the resolve, so it is
// not signalled before this returns.
void PlumeCommandProcessor::ReadbackResolve(Resolved& dest, const draw_util::ResolveInfo& info,
                                            uint32_t base_address, uint32_t dest_x, uint32_t dest_y, uint32_t w,
                                            uint32_t h, uint32_t pitch, uint32_t bpp, uint32_t scale) {
  if (bpp != 4 || !dest.texture || !w || !h || !base_address || !scale) return;
  const bool rgba8 = dest.format == RenderFormat::R8G8B8A8_UNORM;
  const bool half = dest.format == RenderFormat::R16G16B16A16_FLOAT;
  if (!rgba8 && !half) {
    REXLOG_INFO("Renut: Plume resolve readback skipped: host format {}", uint32_t(dest.format));
    return;
  }
  const uint32_t texel = rgba8 ? 4 : 8;
  const uint32_t row_texels = (w + 63) & ~63u;
  auto buffer = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(row_texels) * h * texel));
  if (!buffer) return;
  Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_SOURCE);
  RenderBox box(int32_t(dest_x), int32_t(dest_y), int32_t(dest_x + w), int32_t(dest_y + h), 0, 1);
  CopyTextureToBuffer(L(), buffer.get(), dest.texture.get(), w, h, row_texels, 0, 0, &box);
  Transition(dest.texture.get(), dest.layout, RenderTextureLayout::SHADER_READ);
  Flush();
  const auto* pixels = static_cast<const uint8_t*>(buffer->map());
  if (!pixels) return;
  uint8_t* guest = memory_->TranslatePhysical<uint8_t*>(base_address);
  const bool swap_rb = info.copy_dest_info.copy_dest_swap != 0;
  const uint32_t endian_value = uint32_t(info.copy_dest_info.copy_dest_endian);
  const xenos::Endian endian = endian_value <= 3 ? xenos::Endian(endian_value) : xenos::Endian::k8in32;
  auto to8 = [](uint16_t bits) -> uint32_t {
    const uint32_t exp = (bits >> 10) & 0x1F, mant = bits & 0x3FF;
    if (bits & 0x8000) return 0;
    float v = exp ? std::ldexp(1.f + mant / 1024.f, int(exp) - 15) : std::ldexp(mant / 1024.f, -14);
    if (exp == 31) v = 1.f;
    return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
  };
  uint32_t written = 0;
  const uint32_t limit = dest.guest_bytes;
  // A resolve drawn at a fraction of the guest's resolution is written at the
  // guest's: each guest pixel takes the host texel under it.
  const uint32_t guest_w = uint32_t(uint64_t(w) * 4 / scale), guest_h = uint32_t(uint64_t(h) * 4 / scale);
  const uint32_t guest_x = uint32_t(uint64_t(dest_x) * 4 / scale), guest_y = uint32_t(uint64_t(dest_y) * 4 / scale);
  for (uint32_t gy = 0; gy < guest_h; ++gy) {
    const uint32_t y = std::min(uint32_t(uint64_t(gy) * scale / 4), h - 1);
    const uint8_t* src = pixels + size_t(y) * row_texels * texel;
    for (uint32_t gx = 0; gx < guest_w; ++gx) {
      const uint32_t x = std::min(uint32_t(uint64_t(gx) * scale / 4), w - 1);
      uint32_t r, g, b, a;
      if (rgba8) {
        r = src[x * 4], g = src[x * 4 + 1], b = src[x * 4 + 2], a = src[x * 4 + 3];
      } else {
        const auto* t = reinterpret_cast<const uint16_t*>(src + x * 8);
        r = to8(t[0]), g = to8(t[1]), b = to8(t[2]), a = to8(t[3]);
      }
      if (swap_rb) std::swap(r, b);
      const uint32_t native = r | (g << 8) | (b << 16) | (a << 24);
      const uint32_t value = xenos::GpuSwap(native, endian);
      const uint32_t at = uint32_t(gfx::texture_util::GetTiledOffset2D(int32_t(guest_x + gx), int32_t(guest_y + gy),
                                                                         pitch, 2));
      if (at + 4 > limit) continue;
      std::memcpy(guest + at, &value, 4);
      ++written;
    }
  }
  buffer->unmap();
  // Without this the next lookup would take the changed bytes for another texture
  // streamed into the same memory and drop the resolve.
  dest.guest_hash = ContentHash(memory_->TranslatePhysical<const uint8_t*>(base_address), dest.guest_bytes);
  REXLOG_INFO("Renut: Plume photo capture {} ({}x{} into {:08X}, pitch {}, endian {}, swap {}, {} texels)",
              photo_captures_, w, h, base_address, pitch, endian_value, swap_rb ? 1 : 0, written);
}

bool PlumeCommandProcessor::CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                      RenderTexture* dest, uint32_t dest_x, uint32_t dest_y, bool with_stencil) {
  // Vulkan copies a depth aspect only to a buffer, so go through one: depth
  // to buffer (4 bytes per texel for D32_SFLOAT), buffer to the colour image.
  // With the stencil, the S8 aspect goes to a second buffer and a compute pass
  // joins the two into RG32F texels (depth_stencil_pack.comp).
  constexpr uint64_t kStagingSize = 4096ull * 2048 * 4;
  if (uint64_t(w) * h * 4 > kStagingSize) return false;
  if (!depth_staging_) {
    depth_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        kStagingSize, RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!depth_staging_) return false;
  }
  if (with_stencil && !stencil_staging_) {
    const auto flags = RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE;
    stencil_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize / 4, flags));
    packed_staging_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(kStagingSize * 2, flags));
    if (!stencil_staging_ || !packed_staging_ || !CreatePackPipeline()) {
      REXLOG_WARN("Renut: Plume cannot keep the stencil of depth resolves");
      stencil_staging_.reset();
      packed_staging_.reset();
    }
  }
  with_stencil = with_stencil && packed_staging_ && pack_pipeline_ != VK_NULL_HANDLE;
  // Always a barrier call, which also ends any open render pass before the
  // raw Vulkan commands below.
  L()->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(source.texture.get(), RenderTextureLayout::COPY_SOURCE));
  ++stats_.barriers;
  source.layout = RenderTextureLayout::COPY_SOURCE;
  bound_framebuffer_ = nullptr;
  // plume-skip bit 536870912 keeps that barrier and drops the round trip
  // through the staging buffer, to price it; 268435456 drops every copy.
  if (skip_mask_ & (268435456 | 536870912)) return true;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(L())->vk;
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
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pack_pipeline_);
    vkCmdPushConstants(cb, pack_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (w * h + 63) / 64, 1, 1);
    // The compute layout's push constants may disturb the graphics ones.
    pushed_constants_[0] = pushed_constants_[1] = pushed_constants_[2] = UINT64_MAX;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                             RenderTextureCopyLocation::PlacedFootprint(packed_staging_.get(),
                                                                        RenderFormat::R32G32_FLOAT, w, h, 1, w),
                             dest_x, dest_y);
    return true;
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest),
                           RenderTextureCopyLocation::PlacedFootprint(depth_staging_.get(),
                                                                      RenderFormat::R32_FLOAT, w, h, 1, w),
                           dest_x, dest_y);
  return true;
}

void PlumeCommandProcessor::SnapshotResolved(Resolved& dest) {
  if (!snap_ || snap_width_ != dest.width || snap_height_ != dest.height || snap_format_ != dest.format) {
    snap_ = device_->createTexture(RenderTextureDesc::Texture2D(dest.width, dest.height, 1, dest.format));
    snap_width_ = dest.width;
    snap_height_ = dest.height;
    snap_format_ = dest.format;
    snap_layout_ = RenderTextureLayout::UNKNOWN;
  }
  if (snap_) {
    Transition(dest.texture.get(), dest.layout, RenderTextureLayout::COPY_SOURCE);
    Transition(snap_.get(), snap_layout_, RenderTextureLayout::COPY_DEST);
    L()->copyTexture(snap_.get(), dest.texture.get());
    snap_taken_ = true;
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
void PlumeCommandProcessor::FlushImagesForNextFrame() {
  for (auto& [key, target] : targets_) {
    if (target.texture) Transition(target.texture.get(), target.layout, RenderTextureLayout::SHADER_READ);
  }
  for (auto& [key, r] : resolved_) {
    if (r.texture) Transition(r.texture.get(), r.layout, RenderTextureLayout::SHADER_READ);
    if (r.fitted) Transition(r.fitted.get(), r.fitted_layout, RenderTextureLayout::SHADER_READ);
  }
}

void PlumeCommandProcessor::LogSkinned() {
  std::lock_guard<std::mutex> lock(skinned_mutex_);
  REXLOG_ERROR("Renut: Plume skinned vertex shaders seen: {}", skinned_seen_.size());
  for (const auto& [hash, seen] : skinned_seen_) {
    REXLOG_ERROR("Renut: Plume   skinned vs={:016X} draws={} frames {}..{}", hash, seen.draws, seen.first_frame,
                 seen.last_frame);
  }
}

void PlumeCommandProcessor::ValidateRange(const char* what, uint64_t draw_hash, RenderBuffer* buffer, uint64_t offset,
                                          uint32_t guest_address, uint32_t size, xenos::Endian endian) {
  if (!buffer || buffer == upload_.get() || !size) return;
  std::vector<uint8_t> guest(size);
  CopySwapped(guest.data(), memory_->TranslatePhysical<const uint8_t*>(guest_address), size, endian);
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
  DrainWatchHits();
  if (kept_drop_events_ != drops_before) return;
  ++validate_stale_;
  if (validate_logged_ < 120) {
    ++validate_logged_;
    REXLOG_ERROR("Renut: Plume STALE {} at draw: frame {} vs/ps hash {:016X} guest {:08X}+{:X}: {} bytes differ, first at +{:X}",
                 what, swaps_, draw_hash, guest_address, size, differ, first);
  }
}

}  // namespace renut::gpu
