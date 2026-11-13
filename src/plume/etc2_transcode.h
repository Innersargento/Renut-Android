// BC1-BC5 blocks into the ETC2/EAC blocks of the same size, for a device that samples ETC2 but not BC (the
// Mali-G52). Each 4x4 block is decoded and encoded again on its own, so a level keeps its block count and
// layout and only the upload's format changes:
//
//   BC1        8 bytes  ->  ETC2 RGB8A1 (punch-through alpha: the BC1 transparent texel stays transparent)
//   BC2, BC3  16 bytes  ->  ETC2 RGBA8 (EAC alpha, then the ETC2 colour)
//   BC4        8 bytes  ->  EAC R11
//   BC5       16 bytes  ->  EAC RG11
//
// The colour encoder is ETC1-class (individual and differential modes, both flips; no T, H or planar mode):
// per half block the base is the average colour and the modifier table is searched exhaustively. The EAC
// encoder searches all 16 tables with the multiplier fitted to the block's range. Fast enough to run as a
// level is uploaded; lossy, as a second compression always is.
//
// It works on what a BC block is, not on 16 texels: a colour block has four colours at most, so a half
// block's average and each table's error are sums over those four weighted by how many texels hold each, and
// a texel's modifier is its colour's. A block of one colour, or an alpha block of one value (the opaque
// alpha of most BC3 blocks), is encoded without a search.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace renut::etc {

enum class Source : uint8_t { kBC1, kBC2, kBC3, kBC4, kBC5 };

inline constexpr size_t BlockBytes(Source source) {
  return source == Source::kBC1 || source == Source::kBC4 ? 8 : 16;
}

namespace detail {

inline uint32_t Load16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t Load32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline int Clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// A BC colour block: its four colours and each texel's (row-major, t = y * 4 + x). In BC1's three-colour mode
// the fourth is the transparent texel.
struct BcColors {
  int palette[4][3];
  uint8_t index[16];
  bool transparent = false;  // index 3 is transparent
};

inline void DecodeBcColor(const uint8_t* b, bool four_color, BcColors& out) {
  const uint32_t c0 = Load16(b), c1 = Load16(b + 2), indices = Load32(b + 4);
  const auto expand = [](uint32_t c, int* p) {
    p[0] = int(((c >> 11) & 31) << 3 | ((c >> 11) & 31) >> 2);
    p[1] = int(((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4);
    p[2] = int((c & 31) << 3 | (c & 31) >> 2);
  };
  expand(c0, out.palette[0]);
  expand(c1, out.palette[1]);
  const bool four = four_color || c0 > c1;
  for (int c = 0; c < 3; ++c) {
    if (four) {
      out.palette[2][c] = (2 * out.palette[0][c] + out.palette[1][c] + 1) / 3;
      out.palette[3][c] = (out.palette[0][c] + 2 * out.palette[1][c] + 1) / 3;
    } else {
      out.palette[2][c] = (out.palette[0][c] + out.palette[1][c]) / 2;
      out.palette[3][c] = 0;
    }
  }
  for (int t = 0; t < 16; ++t) out.index[t] = uint8_t((indices >> (2 * t)) & 3);
  out.transparent = !four;
}

// A BC3 alpha / BC4 block into 16 values.
inline void DecodeBcAlpha(const uint8_t* b, uint8_t out[16]) {
  const int a0 = b[0], a1 = b[1];
  int palette[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (int i = 1; i < 5; ++i) palette[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) indices |= uint64_t(b[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t] = uint8_t(palette[(indices >> (3 * t)) & 7]);
}

// BC2's explicit 4-bit alpha into 16 values.
inline void DecodeBc2Alpha(const uint8_t* b, uint8_t out[16]) {
  for (int t = 0; t < 16; ++t) out[t] = uint8_t(((b[t / 2] >> ((t & 1) * 4)) & 15) * 17);
}

inline constexpr int kEtcModifiers[8][2] = {{2, 8},   {5, 17},  {9, 29},   {13, 42},
                                            {18, 60}, {24, 80}, {33, 106}, {47, 183}};
inline constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};
// Perceptual weights of the colour error (about 0.3, 0.6, 0.1).
inline constexpr int kWeight[3] = {3, 6, 1};

struct HalfFit {
  uint32_t error = UINT32_MAX;
  uint8_t table = 0;
  uint8_t pick[4] = {};  // each colour's ETC pixel index value: 0 +a, 1 +b, 2 -a, 3 -b
};

// The best modifier table of a half block around `base`: `count[k]` texels of it hold colour k. With `punch`
// (RGB8A1, not opaque) the values are 0 for index 0, +b and -b (a transparent texel takes index 2, and is in
// no count).
//
// The modifier is added to all three channels alike, so a colour's weighted error is, but for the clamp at 0
// and 255, (W m - n)^2 / W plus what no modifier changes, where W is the weights' sum and n the weighted sum
// of (colour - base). A table's modifiers are two sizes with either sign, so the nearest to n is the nearer
// size with n's sign: no search. The base colour's own error is the same for every table and is added once.
inline HalfFit FitHalf(const int palette[4][3], const uint8_t count[4], const int base[3], bool punch) {
  constexpr int kW = kWeight[0] + kWeight[1] + kWeight[2];
  int projection[4] = {};
  uint32_t fixed = 0;
  for (int k = 0; k < 4; ++k) {
    if (!count[k]) continue;
    int n = 0, square = 0;
    for (int c = 0; c < 3; ++c) {
      const int d = palette[k][c] - base[c];
      n += kWeight[c] * d;
      square += kWeight[c] * d * d;
    }
    projection[k] = n;
    fixed += uint32_t(count[k]) * uint32_t(square - (n * n) / kW);
  }
  HalfFit best;
  for (int table = 0; table < 8; ++table) {
    const int a = kEtcModifiers[table][0] * kW, b = kEtcModifiers[table][1] * kW;
    HalfFit fit;
    fit.table = uint8_t(table);
    uint32_t error = fixed * kW;
    for (int k = 0; k < 4; ++k) {
      if (!count[k]) continue;
      const int n = projection[k], size = std::abs(n);
      int r;
      if (punch) {
        const bool far = 2 * size >= b;
        r = far ? size - b : size;
        fit.pick[k] = uint8_t(far ? (n < 0 ? 3 : 1) : 0);
      } else {
        const bool far = 2 * size >= a + b;
        r = size - (far ? b : a);
        fit.pick[k] = uint8_t((n < 0 ? 2 : 0) | (far ? 1 : 0));
      }
      error += uint32_t(count[k]) * uint32_t(r * r);
    }
    if (error < best.error) {
      fit.error = error;
      best = fit;
    }
  }
  return best;
}

// One ETC2 colour block. `rgb8a1`: the RGB8A1 format, whose mode bit is the opaque flag (always differential);
// a block with a transparent texel is encoded not opaque.
inline void EncodeColor(const BcColors& bc, bool rgb8a1, uint8_t out[8]) {
  // How many texels of each half hold each colour: left/right columns (flip 0), top/bottom rows (flip 1).
  uint8_t count[2][2][4] = {};
  uint32_t used = 0;
  bool punch = false;
  for (int t = 0; t < 16; ++t) {
    const uint32_t k = bc.index[t];
    if (rgb8a1 && bc.transparent && k == 3) {
      punch = true;
      continue;
    }
    ++count[0][(t & 3) >> 1][k];
    ++count[1][t >> 3][k];
    used |= 1u << k;
  }
  // One colour (or none): both ways of halving the block come to the same.
  const int flips = (used & (used - 1)) == 0 && !punch ? 1 : 2;

  uint32_t best_error = UINT32_MAX;
  uint8_t best_bytes[4] = {};
  HalfFit best_fits[2];
  int best_flip = 0;
  for (int flip = 0; flip < flips; ++flip) {
    int average[2][3];
    for (int half = 0; half < 2; ++half) {
      int sum[3] = {}, texels = 0;
      for (int k = 0; k < 4; ++k) {
        const int n = count[flip][half][k];
        texels += n;
        for (int c = 0; c < 3; ++c) sum[c] += n * bc.palette[k][c];
      }
      for (int c = 0; c < 3; ++c) average[half][c] = texels ? (sum[c] + texels / 2) / texels : 0;
    }
    // Differential first: its bases are five bits a channel. The individual mode (four bits) is only worth
    // trying when the second half's base did not fit the difference the differential mode allows.
    bool clamped = false;
    for (int differential = 1; differential >= 0; --differential) {
      if (!differential && (rgb8a1 || !clamped)) break;
      int quantized[2][3], base[2][3];
      for (int c = 0; c < 3; ++c) {
        if (differential) {
          const int q0 = (average[0][c] * 31 + 127) / 255;
          int q1 = (average[1][c] * 31 + 127) / 255;
          const int delta = q1 - q0 < -4 ? -4 : q1 - q0 > 3 ? 3 : q1 - q0;
          clamped = clamped || delta != q1 - q0;
          q1 = q0 + delta;
          quantized[0][c] = q0;
          quantized[1][c] = delta;
          base[0][c] = q0 << 3 | q0 >> 2;
          base[1][c] = q1 << 3 | q1 >> 2;
        } else {
          for (int half = 0; half < 2; ++half) {
            const int q = (average[half][c] * 15 + 127) / 255;
            quantized[half][c] = q;
            base[half][c] = q << 4 | q;
          }
        }
      }
      const HalfFit fits[2] = {FitHalf(bc.palette, count[flip][0], base[0], punch),
                               FitHalf(bc.palette, count[flip][1], base[1], punch)};
      const uint32_t error = fits[0].error + fits[1].error;
      if (error >= best_error) continue;
      best_error = error;
      best_fits[0] = fits[0];
      best_fits[1] = fits[1];
      best_flip = flip;
      for (int c = 0; c < 3; ++c) {
        best_bytes[c] = differential ? uint8_t(quantized[0][c] << 3 | (quantized[1][c] & 7))
                                     : uint8_t(quantized[0][c] << 4 | quantized[1][c]);
      }
      // The mode bit: differential, or for RGB8A1 the opaque flag.
      const int mode_bit = rgb8a1 ? (punch ? 0 : 1) : differential;
      best_bytes[3] = uint8_t(fits[0].table << 5 | fits[1].table << 2 | mode_bit << 1 | flip);
    }
  }
  uint32_t indices = 0;
  for (int t = 0; t < 16; ++t) {
    const uint32_t k = bc.index[t];
    const int half = best_flip ? t >> 3 : (t & 3) >> 1;
    const uint32_t v = rgb8a1 && bc.transparent && k == 3 ? 2u : best_fits[half].pick[k];
    const int p = (t & 3) * 4 + (t >> 2);  // ETC pixel order: column-major
    indices |= (v >> 1) << (16 + p) | (v & 1) << p;
  }
  std::memcpy(out, best_bytes, 4);
  for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t(indices >> (24 - 8 * i));
}

// One EAC block (the ETC2 alpha block; R11 decodes the same codes at 11 bits).
// A decoded BC alpha block has at most 8 distinct values (BC2's at most 16): the search runs over those,
// weighted by how many texels hold each, and the texels' indices are worked out for the winner alone.
inline void EncodeEac(const uint8_t value[16], uint8_t out[8]) {
  int distinct[16], weight[16], count = 0;
  int lo = 255, hi = 0;
  for (int t = 0; t < 16; ++t) {
    const int v = value[t];
    lo = v < lo ? v : lo;
    hi = v > hi ? v : hi;
    int k = 0;
    while (k < count && distinct[k] != v) ++k;
    if (k == count) {
      distinct[count] = v;
      weight[count++] = 0;
    }
    ++weight[k];
  }
  if (count == 1) {
    // One value: the base itself, and the table whose fifth modifier is zero, for every texel.
    uint64_t bits = uint64_t(lo) << 56 | uint64_t(1) << 52 | uint64_t(13) << 48;
    for (int p = 0; p < 16; ++p) bits |= uint64_t(4) << (45 - 3 * p);
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(bits >> (56 - 8 * i));
    return;
  }
  uint32_t best_error = UINT32_MAX;
  int best_table = 0, best_multiplier = 1, best_base = lo;
  for (int table = 0; table < 16 && best_error; ++table) {
    const int* m = kEacModifiers[table];
    const int span = m[7] - m[3];
    const int fitted = ((hi - lo) + span / 2) / span;
    for (int multiplier = fitted; multiplier <= fitted + 1; ++multiplier) {
      if (multiplier < 1 || multiplier > 15) continue;
      const int base = Clamp255(((hi + lo) - multiplier * (m[7] + m[3]) + 1) / 2);
      int levels[8];
      for (int i = 0; i < 8; ++i) levels[i] = Clamp255(base + m[i] * multiplier);
      uint32_t error = 0;
      for (int k = 0; k < count && error < best_error; ++k) {
        uint32_t nearest = UINT32_MAX;
        for (int i = 0; i < 8; ++i) {
          const int d = levels[i] - distinct[k];
          nearest = uint32_t(d * d) < nearest ? uint32_t(d * d) : nearest;
        }
        error += nearest * uint32_t(weight[k]);
      }
      if (error < best_error) {
        best_error = error;
        best_table = table;
        best_multiplier = multiplier;
        best_base = base;
      }
    }
  }
  const int* m = kEacModifiers[best_table];
  uint64_t bits = uint64_t(best_base) << 56 | uint64_t(best_multiplier) << 52 | uint64_t(best_table) << 48;
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      const int v = value[y * 4 + x];
      uint32_t nearest = UINT32_MAX;
      int pick = 0;
      for (int i = 0; i < 8; ++i) {
        const int d = Clamp255(best_base + m[i] * best_multiplier) - v;
        if (uint32_t(d * d) < nearest) {
          nearest = uint32_t(d * d);
          pick = i;
        }
      }
      bits |= uint64_t(pick) << (45 - 3 * (x * 4 + y));
    }
  }
  for (int i = 0; i < 8; ++i) out[i] = uint8_t(bits >> (56 - 8 * i));
}

}  // namespace detail

// `count` blocks of `source` from `in` to `out` (the same size; they may be the same memory). `out` is only
// written, never read: it may be write-combined.
inline void Transcode(Source source, const uint8_t* in, uint8_t* out, size_t count) {
  const size_t bytes = BlockBytes(source);
  for (size_t n = 0; n < count; ++n, in += bytes, out += bytes) {
    uint8_t block[16];
    detail::BcColors colors;
    uint8_t value[16];
    switch (source) {
      case Source::kBC1:
        detail::DecodeBcColor(in, false, colors);
        detail::EncodeColor(colors, true, block);
        break;
      case Source::kBC2:
      case Source::kBC3:
        if (source == Source::kBC2) {
          detail::DecodeBc2Alpha(in, value);
        } else {
          detail::DecodeBcAlpha(in, value);
        }
        detail::DecodeBcColor(in + 8, true, colors);
        detail::EncodeEac(value, block);
        detail::EncodeColor(colors, false, block + 8);
        break;
      case Source::kBC4:
        detail::DecodeBcAlpha(in, value);
        detail::EncodeEac(value, block);
        break;
      case Source::kBC5:
        detail::DecodeBcAlpha(in, value);
        detail::EncodeEac(value, block);
        detail::DecodeBcAlpha(in + 8, value);
        detail::EncodeEac(value, block + 8);
        break;
    }
    std::memcpy(out, block, bytes);
  }
}

}  // namespace renut::etc
