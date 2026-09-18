#include "d3d_layer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_STRING(renut_renderer, "xenos", "reNut",
                      "Renderer: xenos (RexGlue's emulated Xenos GPU) or plume (native "
                      "renderer driven by the game's Direct3D calls; in development)");
REXCVAR_DEFINE_BOOL(renut_d3d_dump_shaders, true, "reNut",
                    "Save every shader container passed to CreateVertexShader and "
                    "CreatePixelShader, for offline recompilation");
REXCVAR_DEFINE_INT32(renut_d3d_report_swaps, 600, "reNut",
                     "Log the most called Direct3D functions every N swaps (0 = off)");

namespace renut::d3d {
namespace {

constexpr const char* kNames[] = {
#define X(symbol, name) #name,
#include "d3d_functions.inc"
#undef X
};

std::array<std::atomic<uint64_t>, kFunctionCount> counts;
std::array<std::atomic<Handler>, kFunctionCount> handlers;
std::atomic<int> plume_mode{-1};

std::mutex dump_mutex;
std::string dump_directory;
std::unordered_set<uint64_t> dumped;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Shader containers begin with flags 0x102A11xx, then virtual and physical
// sizes; the physical part (microcode) follows the virtual part.
void DumpShader(uint8_t* base, uint32_t guest_address, const char* kind) {
  if (!REXCVAR_GET(renut_d3d_dump_shaders) || !guest_address) {
    return;
  }
  const uint8_t* data = base + guest_address;
  uint32_t flags = LoadBE32(data);
  uint64_t size = uint64_t(LoadBE32(data + 4)) + LoadBE32(data + 8);
  if ((flags & 0xFFFFFF00) != 0x102A1100 || size < 36 || size > (16u << 20)) {
    return;
  }
  // FNV-1a: stable across runs, good enough to deduplicate files.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint64_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  }
  std::lock_guard lock(dump_mutex);
  if (dump_directory.empty() || !dumped.insert(hash).second) {
    return;
  }
  auto path = fmt::format("{}/{}_{:016X}.bin", dump_directory, kind, hash);
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(data, 1, size_t(size), file);
    std::fclose(file);
  }
}

void Report() {
  std::vector<std::pair<uint64_t, uint32_t>> sorted;
  uint64_t total = 0;
  for (uint32_t i = 0; i < kFunctionCount; ++i) {
    uint64_t n = counts[i].exchange(0, std::memory_order_relaxed);
    total += n;
    if (n) {
      sorted.emplace_back(n, i);
    }
  }
  std::sort(sorted.rbegin(), sorted.rend());
  std::string line = fmt::format("Renut: D3D calls ({} total, {} functions):", total, sorted.size());
  for (size_t i = 0; i < std::min<size_t>(sorted.size(), 40); ++i) {
    line += fmt::format(" {}={}", kNames[sorted[i].second], sorted[i].first);
  }
  REXLOG_INFO("{}", line);
}

// Called for every intercepted function. Returns true if a handler took it.
inline bool Enter(Function function, PPCContext& ctx, uint8_t* base) {
  counts[function].fetch_add(1, std::memory_order_relaxed);
  if (function == kD3DDevice_CreateVertexShader) {
    DumpShader(base, ctx.r3.u32, "vs");
  } else if (function == kD3DDevice_CreatePixelShader) {
    DumpShader(base, ctx.r3.u32, "ps");
  } else if (function == kD3DDevice_Swap) {
    static std::atomic<uint32_t> swaps{0};
    int32_t every = REXCVAR_GET(renut_d3d_report_swaps);
    if (every > 0 && (++swaps % uint32_t(every)) == 0) {
      Report();
    }
  }
  if (!PlumeActive()) {
    return false;
  }
  Handler handler = handlers[function].load(std::memory_order_acquire);
  return handler && handler(ctx, base);
}

}  // namespace

bool PlumeActive() {
  int mode = plume_mode.load(std::memory_order_relaxed);
  if (mode < 0) {
    mode = REXCVAR_GET(renut_renderer) == "plume" ? 1 : 0;
    plume_mode.store(mode, std::memory_order_relaxed);
  }
  return mode == 1;
}

void SetHandler(Function function, Handler handler) {
  handlers[function].store(handler, std::memory_order_release);
}

void SetShaderDumpDirectory(std::string directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  std::lock_guard lock(dump_mutex);
  dump_directory = std::move(directory);
  // Files from earlier sessions count as dumped.
  for (const auto& entry : std::filesystem::directory_iterator(dump_directory, error)) {
    auto name = entry.path().stem().string();
    if (name.size() > 3) {
      dumped.insert(std::stoull(name.substr(3), nullptr, 16));
    }
  }
}

}  // namespace renut::d3d

// Strong overrides of the weak recompiled definitions.
#define X(symbol, name)                                                          \
  extern "C" void __imp__##symbol(PPCContext& __restrict, uint8_t*);             \
  REX_EXTERN(symbol) {                                                           \
    if (!renut::d3d::Enter(renut::d3d::k##name, ctx, base)) {                    \
      __imp__##symbol(ctx, base);                                                \
    }                                                                            \
  }
#include "d3d_functions.inc"
#undef X
