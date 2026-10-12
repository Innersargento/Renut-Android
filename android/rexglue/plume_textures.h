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

// The guest's block-compressed formats whose blocks are, once byte-swapped, the
// very blocks of a BC format a desktop-class GPU samples (Adreno does; Mali does
// not): DXT1 = BC1, DXT2/3 = BC2, DXT4/5 = BC3, DXT5A = BC4, DXN = BC5. Those
// textures can be uploaded as they are, a quarter to an eighth of the RGBA8 the
// decoder makes, with the fetch constant's swizzle in the view instead.
enum class BcKind : uint8_t { kNone = 0, kBC1, kBC2, kBC3, kBC4, kBC5 };
BcKind BcKindFor(rex::graphics::xenos::TextureFormat format);
// Bytes of one 4x4 block of the host format (8 for BC1/BC4, 16 for the others).
uint32_t BcBlockBytes(BcKind kind);
// The level's blocks as rows of `blocks_x` blocks, untiled and byte-swapped,
// in `blocks` (blocks_x * blocks_y * BcBlockBytes bytes). False when the level
// does not fit the guest memory it was located in.
bool CopyTextureLevelBlocks(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, const TextureLevel& level,
                            const uint8_t* data, std::vector<uint8_t>& blocks);

// Bytes the base level of the texture spans in guest memory (for hashing).
uint32_t TextureBaseSpan(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);

// The base level of a volume texture (dimension k3D, sized by size_3d), every
// Z slice one after the other in `rgba`. `extent` is how many bytes of guest
// memory the level spans from `data`, for hashing; call with data null to get
// the extent and the size alone.
bool DecodeTexture3D(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, const uint8_t* data,
                     std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height,
                     uint32_t& depth, uint32_t& extent);

}  // namespace renut
