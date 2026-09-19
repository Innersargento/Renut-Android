#include "plume_textures.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace renut {
namespace {

namespace xenos = rex::graphics::xenos;
using xenos::TextureFormat;

// One texel as the four components the hardware names X, Y, Z, W (0..255).
struct Texel {
  uint8_t c[4];
};

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Load32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

// Float in [0, 1] (clamped) to 0..255.
uint8_t Unorm(float v) { return uint8_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }

float Half(uint16_t h) {
  const uint32_t sign = h >> 15, exp = (h >> 10) & 31, mant = h & 1023;
  float v = exp == 0 ? std::ldexp(float(mant), -24)
            : exp == 31 ? (mant ? 0.f : 65504.f)
                        : std::ldexp(float(mant | 1024), int(exp) - 25);
  return sign ? -v : v;
}

float Float32(const uint8_t* p) {
  const uint32_t bits = Load32(p);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

uint8_t Expand(uint32_t value, uint32_t bits) {
  return uint8_t(bits >= 8 ? value : (value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

void SwapBlock(uint8_t* block, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(block[i], block[i + 1]);
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(block[i], block[i + 3]);
        std::swap(block[i + 1], block[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(block[i], block[i + 2]);
        std::swap(block[i + 1], block[i + 3]);
      }
      break;
    default:
      break;
  }
}

// DXT colour block (BC1 layout) into 16 texels; `four_color` forces the
// four-colour mode the DXT3/5 colour blocks always use.
void DecodeColorBlock(const uint8_t* b, bool four_color, Texel out[16]) {
  const uint16_t c0 = Load16(b), c1 = Load16(b + 2);
  const uint32_t indices = Load32(b + 4);
  uint8_t palette[4][4];
  auto rgb = [](uint16_t c, uint8_t* p) {
    p[0] = Expand((c >> 11) & 31, 5);
    p[1] = Expand((c >> 5) & 63, 6);
    p[2] = Expand(c & 31, 5);
    p[3] = 255;
  };
  rgb(c0, palette[0]);
  rgb(c1, palette[1]);
  if (four_color || c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i] + 1) / 3);
      palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i] + 1) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2);
    palette[2][3] = 255;
    palette[3][0] = palette[3][1] = palette[3][2] = palette[3][3] = 0;
  }
  for (int t = 0; t < 16; ++t) std::memcpy(out[t].c, palette[(indices >> (2 * t)) & 3], 4);
}

// DXT5-style interpolated alpha block into component `component`.
void DecodeAlphaBlock(const uint8_t* b, Texel out[16], int component) {
  const uint8_t a0 = b[0], a1 = b[1];
  uint8_t palette[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) palette[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i) palette[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) indices |= uint64_t(b[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t].c[component] = palette[(indices >> (3 * t)) & 7];
}

// DXT3 explicit 4-bit alpha into component `component`.
void DecodeExplicitAlpha(const uint8_t* b, Texel out[16], int component) {
  for (int t = 0; t < 16; ++t) {
    uint32_t nibble = (b[t / 2] >> ((t & 1) * 4)) & 0xF;
    out[t].c[component] = uint8_t(nibble * 17);
  }
}

// One block (after byte swapping) into its texels, row-major within the block.
// Returns false for unsupported formats.
bool DecodeBlock(TextureFormat format, const uint8_t* b, Texel* out) {
  switch (format) {
    case TextureFormat::k_8_8_8_8:
    case TextureFormat::k_8_8_8_8_A:
    case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    case TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
      std::memcpy(out[0].c, b, 4);
      return true;
    case TextureFormat::k_5_6_5: {
      uint16_t v = Load16(b);
      out[0] = {{Expand(v & 31, 5), Expand((v >> 5) & 63, 6), Expand(v >> 11, 5), 255}};
      return true;
    }
    case TextureFormat::k_1_5_5_5: {
      uint16_t v = Load16(b);
      out[0] = {{Expand(v & 31, 5), Expand((v >> 5) & 31, 5), Expand((v >> 10) & 31, 5),
                 uint8_t((v >> 15) ? 255 : 0)}};
      return true;
    }
    case TextureFormat::k_4_4_4_4: {
      uint16_t v = Load16(b);
      out[0] = {{Expand(v & 15, 4), Expand((v >> 4) & 15, 4), Expand((v >> 8) & 15, 4), Expand(v >> 12, 4)}};
      return true;
    }
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
    case TextureFormat::k_8_B:
      out[0] = {{b[0], 0, 0, 255}};
      return true;
    case TextureFormat::k_8_8:
      out[0] = {{b[0], b[1], 0, 255}};
      return true;
    // 16-bit and float formats are narrowed to 8 bits: the host texture is
    // RGBA8 for now, so HDR range and precision are lost.
    case TextureFormat::k_16:
    case TextureFormat::k_16_EXPAND:
      out[0] = {{b[1], 0, 0, 255}};
      return true;
    case TextureFormat::k_16_16:
    case TextureFormat::k_16_16_EXPAND:
      out[0] = {{b[1], b[3], 0, 255}};
      return true;
    case TextureFormat::k_16_16_16_16:
    case TextureFormat::k_16_16_16_16_EXPAND:
      out[0] = {{b[1], b[3], b[5], b[7]}};
      return true;
    case TextureFormat::k_16_FLOAT:
      out[0] = {{Unorm(Half(Load16(b))), 0, 0, 255}};
      return true;
    case TextureFormat::k_16_16_FLOAT:
      out[0] = {{Unorm(Half(Load16(b))), Unorm(Half(Load16(b + 2))), 0, 255}};
      return true;
    case TextureFormat::k_16_16_16_16_FLOAT:
      out[0] = {{Unorm(Half(Load16(b))), Unorm(Half(Load16(b + 2))), Unorm(Half(Load16(b + 4))),
                 Unorm(Half(Load16(b + 6)))}};
      return true;
    case TextureFormat::k_32_FLOAT:
      out[0] = {{Unorm(Float32(b)), 0, 0, 255}};
      return true;
    case TextureFormat::k_2_10_10_10: {
      const uint32_t v = Load32(b);
      out[0] = {{uint8_t((v & 1023) >> 2), uint8_t(((v >> 10) & 1023) >> 2),
                 uint8_t(((v >> 20) & 1023) >> 2), Expand(v >> 30, 2)}};
      return true;
    }
    // Depth: sampled as shadow maps. Depth resolves are not copied yet, so
    // report the far plane (nothing shadowed) instead of garbage.
    case TextureFormat::k_24_8:
    case TextureFormat::k_24_8_FLOAT:
      out[0] = {{255, 255, 255, 255}};
      return true;
    case TextureFormat::k_CTX1: {
      // Two 8:8 endpoints (R in the high byte) and 2-bit indices.
      const uint8_t g0 = b[0], r0 = b[1], g1 = b[2], r1 = b[3];
      const uint32_t indices = Load32(b + 4);
      const uint8_t r[4] = {r0, r1, uint8_t((2 * r0 + r1) / 3), uint8_t((r0 + 2 * r1) / 3)};
      const uint8_t g[4] = {g0, g1, uint8_t((2 * g0 + g1) / 3), uint8_t((g0 + 2 * g1) / 3)};
      for (int t = 0; t < 16; ++t) {
        const uint32_t i = (indices >> (2 * t)) & 3;
        out[t] = {{r[i], g[i], 0, 255}};
      }
      return true;
    }
    case TextureFormat::k_DXT1:
    case TextureFormat::k_DXT1_AS_16_16_16_16:
      DecodeColorBlock(b, false, out);
      return true;
    case TextureFormat::k_DXT2_3:
    case TextureFormat::k_DXT2_3_AS_16_16_16_16:
      DecodeColorBlock(b + 8, true, out);
      DecodeExplicitAlpha(b, out, 3);
      return true;
    case TextureFormat::k_DXT4_5:
    case TextureFormat::k_DXT4_5_AS_16_16_16_16:
      DecodeColorBlock(b + 8, true, out);
      DecodeAlphaBlock(b, out, 3);
      return true;
    case TextureFormat::k_DXN:
      for (int t = 0; t < 16; ++t) out[t] = {{0, 0, 0, 255}};
      DecodeAlphaBlock(b, out, 0);
      DecodeAlphaBlock(b + 8, out, 1);
      return true;
    case TextureFormat::k_DXT5A:
      for (int t = 0; t < 16; ++t) out[t] = {{0, 0, 0, 255}};
      DecodeAlphaBlock(b, out, 0);
      return true;
    case TextureFormat::k_DXT3A:
      for (int t = 0; t < 16; ++t) out[t] = {{0, 0, 0, 255}};
      DecodeExplicitAlpha(b, out, 0);
      return true;
    default:
      return false;
  }
}

uint32_t Log2(uint32_t v) { return 31 - __builtin_clz(v); }

}  // namespace

uint32_t TextureBaseSpan(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const auto* info = rex::graphics::FormatInfo::Get(fetch.format);
  if (!info || !info->bytes_per_block()) return 0;
  const uint32_t height = fetch.size_2d.height + 1;
  const uint32_t pitch_blocks = (fetch.pitch * 32 + info->block_width - 1) / info->block_width;
  const uint32_t rows = (height + info->block_height - 1) / info->block_height;
  if (fetch.tiled) {
    return ((pitch_blocks + 31) & ~31u) * ((rows + 31) & ~31u) * info->bytes_per_block();
  }
  return ((pitch_blocks * info->bytes_per_block() + 255) & ~255u) * rows;
}

bool DecodeTexture2D(const xenos::xe_gpu_texture_fetch_t& fetch, const uint8_t* data,
                     std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height) {
  const auto* info = rex::graphics::FormatInfo::Get(fetch.format);
  if (!info || !info->bytes_per_block() || !data) return false;
  const uint32_t bw = info->block_width, bh = info->block_height, bpb = info->bytes_per_block();
  if (bpb > 16 || bw * bh > 16) return false;
  width = fetch.size_2d.width + 1;
  height = fetch.size_2d.height + 1;
  const uint32_t pitch_blocks = (fetch.pitch * 32 + bw - 1) / bw;
  const uint32_t blocks_x = (width + bw - 1) / bw, blocks_y = (height + bh - 1) / bh;
  const uint32_t row_bytes = (pitch_blocks * bpb + 255) & ~255u;
  const uint32_t bpb_log2 = Log2(bpb);

  // The swizzle picks, for R, G, B and A, one of X, Y, Z, W, 0 or 1.
  uint32_t swizzle[4];
  for (int i = 0; i < 4; ++i) swizzle[i] = (fetch.swizzle >> (3 * i)) & 7;

  rgba.assign(size_t(width) * height, 0);
  uint8_t block[16];
  Texel texels[16];
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const size_t offset = fetch.tiled
                                ? size_t(rex::graphics::texture_util::GetTiledOffset2D(
                                      int32_t(bx), int32_t(by), pitch_blocks, bpb_log2))
                                : size_t(by) * row_bytes + size_t(bx) * bpb;
      std::memcpy(block, data + offset, bpb);
      SwapBlock(block, bpb, fetch.endianness);
      if (!DecodeBlock(fetch.format, block, texels)) return false;
      for (uint32_t ty = 0; ty < bh; ++ty) {
        const uint32_t y = by * bh + ty;
        if (y >= height) break;
        for (uint32_t tx = 0; tx < bw; ++tx) {
          const uint32_t x = bx * bw + tx;
          if (x >= width) break;
          const Texel& t = texels[ty * bw + tx];
          uint8_t out[4];
          for (int c = 0; c < 4; ++c) {
            out[c] = swizzle[c] < 4 ? t.c[swizzle[c]] : swizzle[c] == 5 ? 255 : 0;
          }
          std::memcpy(&rgba[size_t(y) * width + x], out, 4);
        }
      }
    }
  }
  return true;
}

}  // namespace renut
