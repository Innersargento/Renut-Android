#pragma once
// CPU decoding of the game's textures for the native Plume renderer: the base
// level of a 2D texture described by a Xenos texture fetch constant, untiled,
// byte-swapped and expanded to RGBA8 with the fetch constant's swizzle applied.
#include <cstdint>
#include <vector>

#include <rex/graphics/xenos.h>

namespace renut {

// `data` is the texture's first byte in host memory (fetch.base_address << 12,
// translated). Returns false for formats this does not decode yet.
bool DecodeTexture2D(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, const uint8_t* data,
                     std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height);

// Where one mip level of a 2D or stacked texture lives, from the guest layout
// (Xenia's GetGuestTextureLayout): levels >= 1 are at mip_address, and levels
// of 16 texels or less share a packed tail tile at per-level block offsets.
struct TextureLevel {
  bool from_mips = false;       // relative to mip_address rather than base_address
  uint32_t offset = 0;          // bytes from that address
  uint32_t extent = 0;          // bytes the level spans (all slices)
  uint32_t row_pitch = 0;       // bytes between block rows
  uint32_t slice_stride = 0;    // bytes between array slices
  uint32_t x_blocks = 0, y_blocks = 0;  // position in the packed tail
  uint32_t width = 0, height = 0;
};
bool LocateTextureLevel(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, uint32_t level,
                        TextureLevel& out);
// Decodes array slice `slice` of the level; `data` points at offset 0 of it
// (the level's start, as LocateTextureLevel found it).
bool DecodeTextureLevel(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                        const TextureLevel& level, const uint8_t* data, uint32_t slice,
                        std::vector<uint32_t>& rgba);

// Bytes the base level of the texture spans in guest memory (for hashing).
uint32_t TextureBaseSpan(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);

}  // namespace renut
