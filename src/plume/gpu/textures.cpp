// Guest textures and samplers.
#include "internal.h"

namespace renut::gpu {

// A texture's content key. The game streams textures into reused memory and
// fills them over several frames, so the start alone says nothing about the
// rest: sample the first kilobyte and 64 slices spread over the whole span.
uint64_t ContentHash(const uint8_t* data, uint32_t span) {
  if (span <= 64 * 1024) return XXH3_64bits(data, span);
  uint64_t hash = XXH3_64bits(data, 1024);
  const uint32_t step = span / 64;
  for (uint32_t i = 1; i <= 64; ++i) {
    const uint32_t at = std::min(i * step, span) - 256;
    hash = XXH3_64bits_withSeed(data + at, 256, hash);
  }
  return hash;
}

// The layout of every level a fetch constant names, and the key the decoded
// texture is kept under. All of it is a pure function of the constant, and the
// draw path asked for it once per texture slot of every draw: the busiest frame
// of Showdown Town ran this arithmetic 169 thousand times, a quarter of the
// command processor's time, for a few hundred distinct textures.
// The layout a fetch constant describes. Asked for the same constant at the
// same slot draw after draw: the slot remembers the six words and what they
// came to, and only a different constant (or a bumped object_epoch_) goes on to
// hash them.
const PlumeCommandProcessor::FetchLayout* PlumeCommandProcessor::LayoutOf(
    const xenos::xe_gpu_texture_fetch_t& fetch) {
  static_assert(sizeof(fetch) == 6 * sizeof(uint32_t));
  if (!objkeys_ || fetch_slot_ < 0) return LayoutOfByContents(fetch);
  SlotObject& object = slot_objects_[uint32_t(fetch_slot_) & 31];
  const uint32_t* words = reinterpret_cast<const uint32_t*>(&fetch);
  if (object.epoch == object_epoch_ && std::memcmp(object.words, words, sizeof(object.words)) == 0) {
    return object.layout;
  }
  const FetchLayout* layout = LayoutOfByContents(fetch);
  std::memcpy(object.words, words, sizeof(object.words));
  object.epoch = object_epoch_;
  object.layout = layout;
  object.texture = nullptr;
  return layout;
}

const PlumeCommandProcessor::FetchLayout* PlumeCommandProcessor::LayoutOfByContents(
    const xenos::xe_gpu_texture_fetch_t& fetch) {
  // The memo key: six words mixed by multiply and fold. Six XXH3 calls for one
  // small input each were a visible slice of the command thread (this runs for
  // every texture slot of every draw whose constant differs from the slot's
  // last). It is kept in memory only (layout_memo_, fetch_layouts_), never
  // written anywhere, so it need not match HashCombine.
  uint64_t fetch_key = 0x9E3779B97F4A7C15ull;
  for (const uint32_t word : {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
                              fetch.dword_5}) {
    fetch_key = (fetch_key ^ word) * 0xFF51AFD7ED558CCDull;
    fetch_key ^= fetch_key >> 32;
  }
  if (const FetchLayout* known = layout_memo_.Find(fetch_key)) return known->usable ? known : nullptr;
  auto found = fetch_layouts_.find(fetch_key);
  if (found != fetch_layouts_.end()) {
    layout_memo_.Put(fetch_key, &found->second);
    return found->second.usable ? &found->second : nullptr;
  }

  FetchLayout& layout = fetch_layouts_[fetch_key];
  layout_memo_.Put(fetch_key, &layout);
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);  // pitch, tiling, (not clamp/sign)
  HashCombine(key, fetch.dword_1);            // format, endian, base address
  HashCombine(key, fetch.dword_2);            // size
  HashCombine(key, fetch.swizzle);
  HashCombine(key, uint32_t(fetch.mip_min_level));
  HashCombine(key, uint32_t(fetch.mip_address));
  uint32_t base_page, mip_page, top_level, last_level;
  gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
      &base_page, &mip_page, &top_level, &last_level);
  if (skip_mask_ & 32768) { base_page = fetch.base_address; top_level = last_level = 0; }
  if (skip_mask_ & 8192) last_level = top_level;
  layout.levels = last_level - top_level + 1;
  layout.top_level = top_level;
  HashCombine(key, last_level);
  HashCombine(key, uint32_t(fetch.packed_mips));
  // The same memory read with and without mips is two host images under
  // texture quality, so the choice is part of the key.
  layout.base_map = fetch.mip_filter == xenos::TextureFilter::kBaseMap;
  HashCombine(key, uint32_t(layout.base_map));
  layout.key = key;
  for (uint32_t i = 0; i < layout.levels; ++i) {
    auto& level = layout.levels_layout[i];
    if (!LocateTextureLevel(fetch, top_level + i, level)) return nullptr;
    const uint64_t start = (uint64_t(level.from_mips ? mip_page : base_page) << 12) + level.offset;
    if (!start || start + level.extent > 0x20000000ull) return nullptr;
    layout.starts[i] = uint32_t(start);
  }
  layout.usable = true;
  return &layout;
}

uint32_t PlumeCommandProcessor::GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address) return 0;
  const FetchLayout* fetch_layout = LayoutOf(fetch);
  if (!fetch_layout) return 0;
  const uint64_t key = fetch_layout->key;
  const uint32_t levels = fetch_layout->levels;
  const std::array<TextureLevel, 14>& layouts = fetch_layout->levels_layout;
  std::array<const uint8_t*, 14> data{};
  uint64_t content = 0;
  for (uint32_t i = 0; i < levels; ++i) {
    data[i] = memory_->TranslatePhysical<const uint8_t*>(fetch_layout->starts[i]);
  }
  // The guest's top mips a low-memory host does not keep: a texture a side of
  // 2048 held as RGBA8 is 21 MB with its chain and 1.4 MB from 512 down, and
  // at this render size nothing shows the difference. Coordinates are
  // normalised, so the shaders sample the smaller image the same way.
  uint32_t skip = 0;
  if (max_texture_dim_) {
    while (skip + 1 < levels && std::max(layouts[skip].width, layouts[skip].height) > max_texture_dim_) ++skip;
  }
  // Texture quality: one mip lower for half, two for quarter, on a texture the
  // guest filters across mips, and never to a side under 64 px. What it reads
  // from its base level alone (the interface, fonts) is not touched.
  if (texture_quality_ && !fetch_layout->base_map) {
    uint32_t reduced = 0;
    while (reduced < texture_quality_ && reduced + 1 < levels &&
           std::max(layouts[reduced + 1].width, layouts[reduced + 1].height) >= 64) {
      ++reduced;
    }
    skip = std::max(skip, reduced);
  }
  const uint32_t host_levels = levels - skip;
  // The slot's own record of the texture its constant resolved to last draw
  // (see SlotObject), checked against the layout it was made for.
  SlotObject* slot_object = objkeys_ && fetch_slot_ >= 0 ? &slot_objects_[uint32_t(fetch_slot_) & 31] : nullptr;
  if (slot_object && (slot_object->epoch != object_epoch_ || slot_object->layout != fetch_layout)) {
    slot_object = nullptr;
  }
  GuestTexture* texture_node = slot_object ? slot_object->texture : nullptr;
  if (!texture_node) {
    texture_node = texture_memo_.Find(key);
    if (!texture_node) {
      texture_node = &guest_textures_[key];
      texture_memo_.Put(key, texture_node);
    }
    if (slot_object) slot_object->texture = texture_node;
  }
  GuestTexture& texture = *texture_node;
  if (texture.failed) return 0;
  // The block format this texture is kept in: its own when the device samples
  // it and the blocks fit, RGBA8 decoded on the CPU otherwise (and always while
  // a texture dump asks for the decoded pixels).
  const BcKind want = bc_supported_ && bc_mode_ && !texture.bc_blocked && texdump_directory_.empty()
                          ? BcKindFor(fetch.format)
                          : BcKind::kNone;
  // Checked once per frame; the draws after the first reuse the result. A
  // texture whose memory is write-watched and was not written since is not
  // checked at all - hashing every texture in use every frame was a tenth of
  // this thread - except once every 256 frames, in case a write went around
  // the watch.
  DrainWatchHits();
  const bool unchanged =
      texture.checked_frame == swaps_ || (texture.watched && ((swaps_ + key) & 255) != 0);
  if (texture.texture && unchanged && texdump_directory_.empty() && texture.bc == want && texture.skip == skip) {
    texture.checked_frame = swaps_;
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  if (keep_streams_ && !texture.watched) {
    // Watch first, hash second, so that a write landing in between is seen.
    texture.watch_begin = {UINT32_MAX, UINT32_MAX};
    texture.watch_end = {0, 0};
    for (uint32_t i = 0; i < levels; ++i) {
      const uint32_t which = layouts[i].from_mips ? 1 : 0;
      texture.watch_begin[which] = std::min(texture.watch_begin[which], fetch_layout->starts[i]);
      texture.watch_end[which] =
          std::max(texture.watch_end[which], fetch_layout->starts[i] + layouts[i].extent);
    }
    for (uint32_t which = 0; which < 2; ++which) {
      if (texture.watch_begin[which] >= texture.watch_end[which]) {
        texture.watch_begin[which] = texture.watch_end[which] = 0;
        continue;
      }
      memory_->EnablePhysicalMemoryAccessCallbacks(texture.watch_begin[which],
                                                   texture.watch_end[which] - texture.watch_begin[which],
                                                   true, false);
      IndexBlocks(texture_block_index_, texture.watch_begin[which],
                  texture.watch_end[which] - texture.watch_begin[which], key);
    }
    texture.watched = true;
  }
  // The mips the host does not keep (see skip) are not read, so not hashed.
  for (uint32_t i = skip; i < levels; ++i) HashCombine(content, ContentHash(data[i], layouts[i].extent));
  if (!texdump_directory_.empty() && dumped_.insert(key).second) texture.content = ~content;
  if (texture.texture && texture.content == content && texture.bc == want && texture.skip == skip) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> rgba;
  std::vector<uint8_t> blocks;
  const uint32_t width = layouts[skip].width, height = layouts[skip].height;
  BcKind kind = want;
  if (kind != BcKind::kNone && !CopyTextureLevelBlocks(fetch, layouts[skip], data[skip], blocks)) {
    // Not a shape a block upload takes: this texture goes through the decoder.
    texture.bc_blocked = true;
    kind = BcKind::kNone;
  }
  if (kind == BcKind::kNone && !DecodeTextureLevel(fetch, layouts[skip], data[skip], 0, rgba)) {
    texture.failed = true;
    ++stats_.texture_failures;
    if (unsupported_formats_.insert(uint32_t(fetch.format)).second) {
      REXLOG_WARN("Renut: Plume does not decode texture format {} yet", uint32_t(fetch.format));
    }
    return 0;
  }
  if (!texdump_directory_.empty()) DumpTexture(fetch, rgba, width, height, "2d");
  RenderFormat host_format = RenderFormat::R8G8B8A8_UNORM;
  switch (kind) {
    case BcKind::kBC1: host_format = RenderFormat::BC1_UNORM; break;
    case BcKind::kBC2: host_format = RenderFormat::BC2_UNORM; break;
    case BcKind::kBC3: host_format = RenderFormat::BC3_UNORM; break;
    case BcKind::kBC4: host_format = RenderFormat::BC4_UNORM; break;
    case BcKind::kBC5: host_format = RenderFormat::BC5_UNORM; break;
    default: break;
  }
  // New contents reuse the image when its shape is unchanged: earlier draws of
  // this frame may still reference it, and the upload is ordered after them.
  if (!texture.texture || texture.levels != host_levels || texture.width != width || texture.height != height ||
      texture.bc != kind) {
    RetireGuestTexture(texture);
    texture.texture = device_->createTexture(RenderTextureDesc::Texture2D(width, height, host_levels, host_format));
    texture.levels = host_levels;
    texture.width = width;
    texture.height = height;
    texture.bc = kind;
    texture.skip = skip;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
    if (kind != BcKind::kNone) {
      // The decoder applied the fetch constant's swizzle to the pixels; the
      // blocks keep the guest's channels, so the view applies it. Identity
      // (X Y Z W, the usual one) needs no view.
      uint32_t swizzle[4];
      bool identity = true;
      for (int i = 0; i < 4; ++i) {
        swizzle[i] = (fetch.swizzle >> (3 * i)) & 7;
        identity = identity && swizzle[i] == uint32_t(i);
      }
      if (!identity) {
        auto pick = [](uint32_t s) {
          switch (s) {
            case 0: return RenderSwizzle::R;
            case 1: return RenderSwizzle::G;
            case 2: return RenderSwizzle::B;
            case 3: return RenderSwizzle::A;
            case 5: return RenderSwizzle::ONE;
            default: return RenderSwizzle::ZERO;
          }
        };
        RenderTextureViewDesc view = RenderTextureViewDesc::Texture2D(host_format);
        view.componentMapping = RenderComponentMapping(pick(swizzle[0]), pick(swizzle[1]), pick(swizzle[2]),
                                                       pick(swizzle[3]));
        texture.view = texture.texture->createTextureView(view);
      }
    }
  }
  texture.content = content;
  if (!texture.descriptor) texture.descriptor = AllocateTextureDescriptor();
  if (!texture.descriptor) {
    // Out of descriptors: try again next frame, once eviction has freed some,
    // rather than keep this texture white for as long as it stays in use.
    texture.content = ~content;
    return 0;
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  uint32_t w = width, h = height;
  for (uint32_t level = 0; level < host_levels; ++level) {
    w = layouts[skip + level].width;
    h = layouts[skip + level].height;
    if (kind != BcKind::kNone) {
      // The guest's blocks as they are; a level that does not fit its memory
      // (not seen) stays zero blocks rather than leave the image half written.
      if (level && !CopyTextureLevelBlocks(fetch, layouts[skip + level], data[skip + level], blocks)) {
        blocks.assign(size_t((w + 3) / 4) * ((h + 3) / 4) * BcBlockBytes(kind), 0);
      }
      const uint64_t staging = Allocate(blocks.size(), 512);
      if (staging == UINT64_MAX) return 0;
      std::memcpy(Pointer(staging), blocks.data(), blocks.size());
      // The row is counted in texels and a multiple of the 4-texel block.
      L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), level),
                               RenderTextureCopyLocation::PlacedFootprint(
                                   upload_.get(), host_format, w, h, 1, ((w + 3) / 4) * 4, staging));
      continue;
    }
    if (level && !DecodeTextureLevel(fetch, layouts[skip + level], data[skip + level], 0, rgba)) return 0;
    const uint64_t staging = Allocate(uint64_t(w) * h * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), rgba.data(), size_t(w) * h * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), level),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, w, h, 1, w, staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  SetTexture2D(texture.descriptor, texture.texture.get(), texture.view.get());
  ++stats_.textures_decoded;
  if (kind != BcKind::kNone) ++stats_.textures_bc;
  return texture.descriptor;
}

void PlumeCommandProcessor::DumpTexture(const xenos::xe_gpu_texture_fetch_t& fetch,
                                        const std::vector<uint32_t>& rgba, uint32_t width,
                                        uint32_t height, const char* kind) {
  static uint32_t serial = 0;
  const auto path = fmt::format(
      "{}/{:04}_{}_{:08X}_f{}_{}x{}_t{}_e{}_p{}_sw{:03X}_s{}{}{}{}_n{}_m{}_mn{}_ma{:08X}_pk{}.rgba", texdump_directory_, serial++,
      kind, fetch.base_address << 12, uint32_t(fetch.format), width, height, uint32_t(fetch.tiled),
      uint32_t(fetch.endianness), uint32_t(fetch.pitch), uint32_t(fetch.swizzle), uint32_t(fetch.sign_x),
      uint32_t(fetch.sign_y), uint32_t(fetch.sign_z), uint32_t(fetch.sign_w), uint32_t(fetch.num_format),
      uint32_t(fetch.mip_max_level), uint32_t(fetch.mip_min_level), fetch.mip_address << 12,
      uint32_t(fetch.packed_mips));
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(rgba.data(), 4, size_t(width) * height, file);
    std::fclose(file);
  }
}

// The sampler a fetch constant asks for. The guest states every axis on its
// own - magnification, minification and mip filters, anisotropy, a clamp mode
// per axis and a LOD bias - and Plume used to reduce all of that to "point or
// linear" and the X axis's wrap mode, which cost the textures their sharpness
// at grazing angles and addressed Y and Z with a mode meant for X.
//
// One exception: a stacked texture's Z picks a slice of an array, and the guest
// clamps a layer to the ends of the stack whatever the mode reads.
uint32_t PlumeCommandProcessor::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t key = uint32_t(fetch.mag_filter) | (uint32_t(fetch.min_filter) << 2) |
                       (uint32_t(fetch.mip_filter) << 4) | (uint32_t(fetch.aniso_filter) << 6) |
                       (uint32_t(fetch.clamp_x) << 9) | (uint32_t(fetch.clamp_y) << 12) |
                       (uint32_t(fetch.clamp_z) << 15) | (uint32_t(fetch.stacked) << 18) |
                       ((uint32_t(fetch.lod_bias) & 0x3FF) << 19);
  // The fallback is the old behaviour: linear everywhere, no anisotropy, and
  // every axis wrapped, for comparing a capture against.
  if (skip_mask_ & 524288) return kSamplerDefault;
  if (const uint32_t* memo = sampler_memo_.Find(key)) return *memo;
  auto found = sampler_indices_.find(key);
  if (found != sampler_indices_.end()) {
    sampler_memo_.Put(key, &found->second);
    return found->second;
  }
  if (next_sampler_ >= kMaxSamplers) return kSamplerDefault;

  auto filter = [](xenos::TextureFilter f) {
    return f == xenos::TextureFilter::kPoint ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  };
  auto address = [](xenos::ClampMode clamp) {
    switch (clamp) {
      case xenos::ClampMode::kRepeat: return RenderTextureAddressMode::WRAP;
      case xenos::ClampMode::kMirroredRepeat: return RenderTextureAddressMode::MIRROR;
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway:
        return RenderTextureAddressMode::MIRROR_ONCE;
      case xenos::ClampMode::kClampToBorder:
      case xenos::ClampMode::kMirrorClampToBorder:
        return RenderTextureAddressMode::BORDER;
      default: return RenderTextureAddressMode::CLAMP;
    }
  };

  RenderSamplerDesc desc;
  desc.magFilter = filter(fetch.mag_filter);
  desc.minFilter = filter(fetch.min_filter);
  // kBaseMap is the guest asking for the top level alone, not for a mip filter.
  desc.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear
                        ? RenderMipmapMode::LINEAR
                        : RenderMipmapMode::NEAREST;
  if (fetch.mip_filter == xenos::TextureFilter::kBaseMap) desc.maxLOD = 0.0f;
  desc.addressU = address(fetch.clamp_x);
  desc.addressV = address(fetch.clamp_y);
  desc.addressW = fetch.stacked ? RenderTextureAddressMode::CLAMP : address(fetch.clamp_z);
  // The bias has five fractional bits.
  desc.mipLODBias = float(fetch.lod_bias) / 32.0f;
  if (fetch.aniso_filter != xenos::AnisoFilter::kDisabled &&
      fetch.aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (uint32_t(fetch.aniso_filter) - 1);
  }
  if (xenos::ClampModeUsesBorder(fetch.clamp_x) || xenos::ClampModeUsesBorder(fetch.clamp_y) ||
      xenos::ClampModeUsesBorder(fetch.clamp_z)) {
    desc.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White
                           ? RenderBorderColor::OPAQUE_WHITE
                           : RenderBorderColor::TRANSPARENT_BLACK;
  }

  std::unique_ptr<RenderSampler> sampler = device_->createSampler(desc);
  if (!sampler) return kSamplerDefault;
  const uint32_t index = next_sampler_++;
  samplers_[index] = std::move(sampler);
  set_samplers_->setSampler(index, samplers_[index].get());
  sampler_indices_.emplace(key, index);
  return index;
}

// Stacked textures (arrays of 2D slices: the terrain's material and normal map
// sets, and the fur bump map on Banjo, whose 16 slices are one relief at
// growing amplitude), which the shaders sample with tfetch3D, z picking the
// slice. Slices follow each other at a 4 KB aligned stride, bound as a 3D
// texture of that depth.
//
// A host 3D image halves its depth at every mip level, which would blend one
// slice into the next, so only a stack one slice deep can carry the guest's mip
// chain; a deeper stack keeps the single level it had before, as every stack in
// the menu does.
uint32_t PlumeCommandProcessor::GetGuestStackedTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address || (skip_mask_ & 65536)) return 0;
  const uint32_t depth = uint32_t(fetch.size_2d.stack_depth) + 1;
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);
  HashCombine(key, fetch.dword_1);
  HashCombine(key, fetch.dword_2);
  HashCombine(key, fetch.swizzle);
  HashCombine(key, uint32_t(fetch.mip_min_level));
  HashCombine(key, uint32_t(fetch.mip_address));
  uint32_t base_page, mip_page, top_level, last_level;
  gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
      &base_page, &mip_page, &top_level, &last_level);
  if (skip_mask_ & 32768) { base_page = fetch.base_address; top_level = last_level = 0; }
  if (depth > 1 || (skip_mask_ & (8192 | 262144))) last_level = top_level;
  const uint32_t levels = last_level - top_level + 1;
  HashCombine(key, last_level);
  HashCombine(key, uint32_t(fetch.packed_mips));
  std::array<TextureLevel, 14> layouts;
  std::array<const uint8_t*, 14> data{};
  for (uint32_t i = 0; i < levels; ++i) {
    TextureLevel& level = layouts[i];
    if (!LocateTextureLevel(fetch, top_level + i, level)) return 0;
    const uint64_t start = (uint64_t(level.from_mips ? mip_page : base_page) << 12) + level.offset;
    if (!start || start + level.extent > 0x20000000ull) return 0;
    data[i] = memory_->TranslatePhysical<const uint8_t*>(uint32_t(start));
  }
  GuestTexture& texture = guest_stacks_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_ && texdump_directory_.empty()) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  uint64_t content = 0;
  for (uint32_t i = 0; i < levels; ++i) HashCombine(content, ContentHash(data[i], layouts[i].extent));
  if (!texdump_directory_.empty() && dumped_.insert(key ^ 2).second) texture.content = ~content;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  // Every level is decoded before the image is touched: a level that fails
  // partway has to leave the contents an earlier frame uploaded alone.
  std::array<std::vector<uint32_t>, 14> all;
  std::vector<uint32_t> slice;
  for (uint32_t i = 0; i < levels; ++i) {
    for (uint32_t z = 0; z < depth; ++z) {
      if (!DecodeTextureLevel(fetch, layouts[i], data[i], z, slice)) {
        texture.failed = true;
        ++stats_.texture_failures;
        return 0;
      }
      if (!texdump_directory_.empty()) {
        DumpTexture(fetch, slice, layouts[i].width, layouts[i].height, "stack");
      }
      all[i].insert(all[i].end(), slice.begin(), slice.end());
    }
  }
  const uint32_t width = layouts[0].width, height = layouts[0].height;
  if (!texture.descriptor) {
    texture.descriptor = Allocate3DDescriptor();
    if (!texture.descriptor) return 0;
  }
  if (!texture.texture || texture.width != width || texture.height != height ||
      texture.depth != depth || texture.levels != levels) {
    if (texture.texture) retired_slots_[slot_].push_back(std::move(texture.texture));
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture3D(width, height, depth, levels, RenderFormat::R8G8B8A8_UNORM));
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.levels = levels;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
  }
  texture.content = content;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < levels; ++i) {
    const uint32_t w = layouts[i].width, h = layouts[i].height;
    const uint64_t staging = Allocate(uint64_t(w) * h * depth * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), all[i].data(), all[i].size() * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), i),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, w, h, depth, w,
                                 staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_3d_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_.textures_decoded;
  return texture.descriptor;
}

// Volume textures (dimension k3D). The particle simulations draw their wind
// and turbulence from one: D38D10B70F954F86 turns a texel of it into the push
// every falling leaf gets each frame. Unbound, the slot read the white
// fallback, the same (1, 1, 1) push for every leaf, and the leaves streamed
// off along one diagonal into sheets across the screen. Only the base level:
// the vertex shaders that sample these do so at level 0, and a host 3D image
// would halve its depth at each mip anyway.
uint32_t PlumeCommandProcessor::GetGuestVolumeTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address) return 0;
  std::vector<uint32_t> rgba;
  uint32_t width = 0, height = 0, depth = 0, extent = 0;
  if (!DecodeTexture3D(fetch, nullptr, rgba, width, height, depth, extent) ||
      uint64_t(address) + extent > 0x20000000ull) {
    return 0;
  }
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);
  HashCombine(key, fetch.dword_1);
  HashCombine(key, fetch.dword_2);
  HashCombine(key, fetch.swizzle);
  const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(address);
  GuestTexture& texture = guest_volumes_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  const uint64_t content = ContentHash(data, extent);
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  if (!DecodeTexture3D(fetch, data, rgba, width, height, depth, extent)) {
    texture.failed = true;
    ++stats_.texture_failures;
    REXLOG_WARN("Renut: Plume could not decode volume texture {:08X} f{} {}x{}x{}", address,
                uint32_t(fetch.format), width, height, depth);
    return 0;
  }
  if (!texture.descriptor) {
    texture.descriptor = Allocate3DDescriptor();
    if (!texture.descriptor) return 0;
  }
  if (!texture.texture || texture.width != width || texture.height != height || texture.depth != depth) {
    if (texture.texture) retired_slots_[slot_].push_back(std::move(texture.texture));
    texture.texture = device_->createTexture(
        RenderTextureDesc::Texture3D(width, height, depth, 1, RenderFormat::R8G8B8A8_UNORM));
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.levels = 1;
    texture.layout = RenderTextureLayout::UNKNOWN;
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
    REXLOG_INFO("Renut: Plume volume texture {:08X} f{} {}x{}x{} tiled={}", address, uint32_t(fetch.format),
                width, height, depth, uint32_t(fetch.tiled));
  }
  texture.content = content;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  const uint64_t staging = Allocate(uint64_t(rgba.size()) * 4, 512);
  if (staging == UINT64_MAX) return 0;
  std::memcpy(Pointer(staging), rgba.data(), rgba.size() * 4);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), 0),
                           RenderTextureCopyLocation::PlacedFootprint(
                               upload_.get(), RenderFormat::R8G8B8A8_UNORM, width, height, depth, width,
                               staging));
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_3d_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_.textures_decoded;
  return texture.descriptor;
}

// Cube maps (the sky dome): six 2D faces, +X -X +Y -Y +Z -Z, one after the
// other at a 4 KB aligned stride, each decoded like a 2D texture.
uint32_t PlumeCommandProcessor::GetGuestCubeTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t address = fetch.base_address << 12;
  if (!address || (skip_mask_ & 16384)) return 0;
  uint64_t key = 0;
  HashCombine(key, fetch.dword_0 & ~0x3FFu);
  HashCombine(key, fetch.dword_1);
  HashCombine(key, fetch.dword_2);
  HashCombine(key, fetch.swizzle);
  const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(address);
  const uint32_t face_stride = (TextureBaseSpan(fetch) + 4095) & ~4095u;
  GuestTexture& texture = guest_cubes_[key];
  if (texture.failed) return 0;
  if (texture.texture && texture.checked_frame == swaps_ && texdump_directory_.empty()) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  texture.checked_frame = swaps_;
  const uint64_t content = ContentHash(data, face_stride * 6);
  if (!texdump_directory_.empty() && dumped_.insert(key ^ 1).second) texture.content = ~content;
  if (texture.texture && texture.content == content) {
    Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
    return texture.descriptor;
  }
  std::vector<uint32_t> faces[6];
  uint32_t width = 0, height = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    uint32_t w = 0, h = 0;
    if (!face_stride || !DecodeTexture2D(fetch, data + size_t(face) * face_stride, faces[face], w, h) ||
        (face && (w != width || h != height)) || w != h) {
      texture.failed = true;
      ++stats_.texture_failures;
      return 0;
    }
    width = w;
    height = h;
  }
  if (!texdump_directory_.empty()) {
    for (uint32_t face = 0; face < 6; ++face) DumpTexture(fetch, faces[face], width, height, "cube");
  }
  if (!texture.descriptor) {
    texture.descriptor = AllocateCubeDescriptor();
    if (!texture.descriptor) return 0;
  }
  if (!texture.texture) {
    texture.texture = device_->createTexture(RenderTextureDesc::Texture(
        RenderTextureDimension::TEXTURE_2D, width, height, 1, 1, 6, RenderFormat::R8G8B8A8_UNORM,
        RenderTextureFlag::CUBE));
    if (!texture.texture) {
      texture.failed = true;
      return 0;
    }
    RenderTextureViewDesc view;
    view.format = RenderFormat::R8G8B8A8_UNORM;
    view.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
    texture.view = texture.texture->createTextureView(view);
    texture.layout = RenderTextureLayout::UNKNOWN;
  }
  texture.content = content;
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::COPY_DEST);
  for (uint32_t face = 0; face < 6; ++face) {
    const uint64_t staging = Allocate(uint64_t(width) * height * 4, 512);
    if (staging == UINT64_MAX) return 0;
    std::memcpy(Pointer(staging), faces[face].data(), size_t(width) * height * 4);
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), 0, face),
                             RenderTextureCopyLocation::PlacedFootprint(
                                 upload_.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, width,
                                 staging));
  }
  Transition(texture.texture.get(), texture.layout, RenderTextureLayout::SHADER_READ);
  set_cube_->setTexture(texture.descriptor, texture.texture.get(), RenderTextureLayout::SHADER_READ,
                        texture.view.get());
  ++stats_.textures_decoded;
  return texture.descriptor;
}

}  // namespace renut::gpu
