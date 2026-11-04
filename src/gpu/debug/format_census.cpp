#include "gpu/debug/format_census.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>

namespace renut::gpu::format_census {
namespace {

// TextureFormat names, indexed by value (VertexFormat uses the same numbers).
constexpr const char* kFormatNames[64] = {
    "k_1_REVERSE", "k_1", "k_8", "k_1_5_5_5", "k_5_6_5", "k_6_5_5", "k_8_8_8_8", "k_2_10_10_10",
    "k_8_A", "k_8_B", "k_8_8", "k_Cr_Y1_Cb_Y0_REP", "k_Y1_Cr_Y0_Cb_REP", "k_16_16_EDRAM", "k_8_8_8_8_A", "k_4_4_4_4",
    "k_10_11_11", "k_11_11_10", "k_DXT1", "k_DXT2_3", "k_DXT4_5", "k_16_16_16_16_EDRAM", "k_24_8", "k_24_8_FLOAT",
    "k_16", "k_16_16", "k_16_16_16_16", "k_16_EXPAND", "k_16_16_EXPAND", "k_16_16_16_16_EXPAND", "k_16_FLOAT", "k_16_16_FLOAT",
    "k_16_16_16_16_FLOAT", "k_32", "k_32_32", "k_32_32_32_32", "k_32_FLOAT", "k_32_32_FLOAT", "k_32_32_32_32_FLOAT", "k_32_AS_8",
    "k_32_AS_8_8", "k_16_MPEG", "k_16_16_MPEG", "k_8_INTERLACED", "k_32_AS_8_INTERLACED", "k_32_AS_8_8_INTERLACED", "k_16_INTERLACED", "k_16_MPEG_INTERLACED",
    "k_16_16_MPEG_INTERLACED", "k_DXN", "k_8_8_8_8_AS_16_16_16_16", "k_DXT1_AS_16_16_16_16", "k_DXT2_3_AS_16_16_16_16", "k_DXT4_5_AS_16_16_16_16",
    "k_2_10_10_10_AS_16_16_16_16", "k_10_11_11_AS_16_16_16_16", "k_11_11_10_AS_16_16_16_16", "k_32_32_32_FLOAT", "k_DXT3A", "k_DXT5A", "k_CTX1",
    "k_DXT3A_AS_1_1_1_1",
};
// 62 and 63 do not fit the table above without a gap: handled in FormatName.

const char* FormatName(uint32_t value) {
  if (value == 62) return "k_8_8_8_8_GAMMA_EDRAM";
  if (value == 63) return "k_2_10_10_10_FLOAT_EDRAM";
  return value < 62 ? kFormatNames[value] : "?";
}

const char* ColorTargetName(uint32_t v) {
  switch (v) {
    case 0: return "k_8_8_8_8";
    case 1: return "k_8_8_8_8_GAMMA";
    case 2: return "k_2_10_10_10";
    case 3: return "k_2_10_10_10_FLOAT";
    case 4: return "k_16_16";
    case 5: return "k_16_16_16_16";
    case 6: return "k_16_16_FLOAT";
    case 7: return "k_16_16_16_16_FLOAT";
    case 10: return "k_2_10_10_10_AS_10_10_10_10";
    case 12: return "k_2_10_10_10_FLOAT_AS_16_16_16_16";
    case 14: return "k_32_FLOAT";
    case 15: return "k_32_32_FLOAT";
    default: return "?";
  }
}

const char* PrimitiveName(uint32_t v) {
  switch (v) {
    case 0x00: return "kNone";
    case 0x01: return "kPointList";
    case 0x02: return "kLineList";
    case 0x03: return "kLineStrip";
    case 0x04: return "kTriangleList";
    case 0x05: return "kTriangleFan";
    case 0x06: return "kTriangleStrip";
    case 0x07: return "kTriangleWithWFlags";
    case 0x08: return "kRectangleList";
    case 0x0C: return "kLineLoop";
    case 0x0D: return "kQuadList";
    case 0x0E: return "kQuadStrip";
    case 0x0F: return "kPolygon";
    case 0x10: return "k2DCopyRectListV0/kLinePatch";
    case 0x11: return "k2DCopyRectListV1/kTrianglePatch";
    case 0x12: return "k2DCopyRectListV2/kQuadPatch";
    case 0x13: return "k2DCopyRectListV3";
    case 0x14: return "k2DFillRectList";
    case 0x15: return "k2DLineStrip";
    case 0x16: return "k2DTriStrip";
    default: return "?";
  }
}

const char* DimensionName(uint32_t v) {
  static const char* names[] = {"1D", "2DOrStacked", "3D", "Cube"};
  return names[v & 3];
}

std::mutex g_mutex;
// texture: (format, dimension, tiled, packed_mips, stacked, endianness, mip levels > 1)
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> g_textures;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> g_vertices;  // format, signed, normalized
std::map<uint32_t, uint64_t> g_resolves, g_color_targets, g_depth_targets;
std::array<std::atomic<uint64_t>, 32> g_prims{};
std::array<std::atomic<uint64_t>, 5> g_index_sizes{};  // index 0: none, 2, 4 bytes at their value

}  // namespace

void Texture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch) {
  const auto key = std::make_tuple(uint32_t(fetch.format), uint32_t(fetch.dimension), uint32_t(fetch.tiled),
                                   uint32_t(fetch.packed_mips), uint32_t(fetch.stacked), uint32_t(fetch.endianness),
                                   uint32_t(fetch.mip_max_level > fetch.mip_min_level ? 1 : 0));
  std::lock_guard lock(g_mutex);
  ++g_textures[key];
}

void Vertex(uint32_t format, bool is_signed, bool normalized) {
  std::lock_guard lock(g_mutex);
  ++g_vertices[{format, is_signed, normalized}];
}

void Resolve(uint32_t dest_format) {
  std::lock_guard lock(g_mutex);
  ++g_resolves[dest_format];
}

void ColorTarget(uint32_t format) {
  std::lock_guard lock(g_mutex);
  ++g_color_targets[format];
}

void DepthTarget(uint32_t format) {
  std::lock_guard lock(g_mutex);
  ++g_depth_targets[format];
}

void Primitive(uint32_t prim_type, uint32_t index_size_bytes) {
  g_prims[prim_type & 31].fetch_add(1, std::memory_order_relaxed);
  g_index_sizes[index_size_bytes > 4 ? 0 : index_size_bytes].fetch_add(1, std::memory_order_relaxed);
}

void Dump(const std::string& miss_dump_directory, uint64_t swap) {
  if (miss_dump_directory.empty()) return;
  const std::string path = miss_dump_directory.substr(0, miss_dump_directory.rfind('/')) + "/used-formats.txt";
  FILE* out = std::fopen(path.c_str(), "w");
  if (!out) return;
  std::fprintf(out, "# format census at swap %llu\n", (unsigned long long)swap);
  std::fprintf(out, "# counts are per distinct texture constant / per shader / per target, not per draw\n\n");
  {
    std::lock_guard lock(g_mutex);
    std::fprintf(out, "[textures] format dim tiled packed_mips stacked endian has_mips -> count\n");
    for (const auto& [k, n] : g_textures) {
      std::fprintf(out, "%-28s %-12s tiled=%u packed=%u stacked=%u endian=%u mips=%u  %llu\n",
                   FormatName(std::get<0>(k)), DimensionName(std::get<1>(k)), std::get<2>(k), std::get<3>(k),
                   std::get<4>(k), std::get<5>(k), std::get<6>(k), (unsigned long long)n);
    }
    std::fprintf(out, "\n[vertex elements] format signed normalized -> shaders\n");
    for (const auto& [k, n] : g_vertices) {
      std::fprintf(out, "%-28s signed=%u normalized=%u  %llu\n", FormatName(std::get<0>(k)), std::get<1>(k),
                   std::get<2>(k), (unsigned long long)n);
    }
    std::fprintf(out, "\n[resolve destination formats]\n");
    for (const auto& [k, n] : g_resolves) std::fprintf(out, "%-28s %llu\n", FormatName(k), (unsigned long long)n);
    std::fprintf(out, "\n[color render targets]\n");
    for (const auto& [k, n] : g_color_targets) std::fprintf(out, "%-36s %llu\n", ColorTargetName(k), (unsigned long long)n);
    std::fprintf(out, "\n[depth render targets]\n");
    for (const auto& [k, n] : g_depth_targets) {
      std::fprintf(out, "%-28s %llu\n", k ? "kD24FS8" : "kD24S8", (unsigned long long)n);
    }
  }
  std::fprintf(out, "\n[primitive types] per draw\n");
  for (uint32_t i = 0; i < g_prims.size(); ++i) {
    const uint64_t n = g_prims[i].load(std::memory_order_relaxed);
    if (n) std::fprintf(out, "0x%02X %-34s %llu\n", i, PrimitiveName(i), (unsigned long long)n);
  }
  std::fprintf(out, "\n[index size in bytes, 0 = none] per draw\n");
  for (uint32_t i = 0; i < g_index_sizes.size(); ++i) {
    const uint64_t n = g_index_sizes[i].load(std::memory_order_relaxed);
    if (n) std::fprintf(out, "%u %llu\n", i, (unsigned long long)n);
  }
  std::fclose(out);
}

}  // namespace renut::gpu::format_census
