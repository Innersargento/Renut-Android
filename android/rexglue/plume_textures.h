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

// Bytes the base level of the texture spans in guest memory (for hashing).
uint32_t TextureBaseSpan(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);

}  // namespace renut
