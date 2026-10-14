#include "d3d_layer.h"
#include "native_d3d.h"
#include "native_probe.h"
#include "guest/guest_address.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
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
#include <rex/graphics/command_processor.h>
#include <rex/system/kernel_state.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(renut_d3d_dump_shaders, true, "reNut",
                    "Save every shader the game creates or registers (Create*Shader, "
                    "XGRegister*Shader) as a complete container, for offline recompilation");
REXCVAR_DEFINE_BOOL(renut_single_tile, false, "reNut",
                    "Draw the MSAA scene in one pass instead of the bands (predicated tiling) the "
                    "Xbox 360 needs for its 10 MB of EDRAM; the game still sets up its bands");
REXCVAR_DEFINE_INT32(renut_d3d_report_swaps, 0, "reNut",
                     "Log the most called Direct3D functions every N swaps (0 = off). Off by "
                     "default: the game polls D3D_CBlocker_Check tens of millions of times a "
                     "second while it waits for a fence, and counting every call cost a quarter "
                     "of the process's cycles in one atomic increment");

namespace renut::d3d {
namespace {

constexpr const char* kNames[] = {
#define X(symbol, name) #name,
#include "d3d_functions.inc"
#undef X
};

std::array<std::atomic<uint64_t>, kFunctionCount> counts;
std::array<std::atomic<Handler>, kFunctionCount> handlers;
// renut_d3d_report_swaps, read once: every intercepted call tests it.
std::atomic<int32_t> report_every{-1};

inline int32_t ReportEvery() {
  int32_t every = report_every.load(std::memory_order_relaxed);
  if (every < 0) {
    every = std::max(0, REXCVAR_GET(renut_d3d_report_swaps));
    report_every.store(every, std::memory_order_relaxed);
  }
  return every;
}

std::mutex dump_mutex;
std::string dump_directory;
std::unordered_set<uint64_t> dumped;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool IsContainer(const uint8_t* data) {
  return (LoadBE32(data) & 0xFFFFFF00) == 0x102A1100 && LoadBE32(data + 28) == 0 &&
         LoadBE32(data + 32) == 0;
}

// Saves a complete shader container - the virtual part the object was built
// from, followed by the physical part (literals and microcode) the GPU reads -
// once the object is registered, so the microcode is byte for byte what the
// command processor is handed (XGRegisterVertexShader patches it).
//
// Object layout, from the recompiled XGRegisterVertexShader/PixelShader and
// XGGetMicrocodeShaderParts: a vertex shader object is 872 bytes, a pixel
// shader object 40, and the container's virtual part follows the object. The
// physical part's guest address is stored at +32 (vertex) or +24 (pixel).
void DumpShaderObject(uint8_t* base, uint32_t object, bool vertex) {
  if (!REXCVAR_GET(renut_d3d_dump_shaders) || !object) {
    return;
  }
  uint32_t container = 0;
  for (uint32_t offset : {vertex ? 872u : 40u, vertex ? 40u : 872u}) {
    if (IsContainer(renut::GuestPtr(base, object + offset))) {
      container = object + offset;
      break;
    }
  }
  if (!container) {
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1) < 8) {
      REXLOG_WARN("Renut: shader object {:08X} has no container after it", object);
    }
    return;
  }
  const uint8_t* header = renut::GuestPtr(base, container);
  uint32_t virtual_size = LoadBE32(header + 4);
  uint32_t physical_size = LoadBE32(header + 8);
  uint32_t physical = LoadBE32(renut::GuestPtr(base, object + (vertex ? 32 : 24)));
  if (!physical || virtual_size < 36 || virtual_size > (1u << 20) || physical_size > (1u << 20)) {
    return;
  }
  std::vector<uint8_t> blob(header, header + virtual_size);
  blob.insert(blob.end(), renut::GuestPtr(base, physical), renut::GuestPtr(base, physical) + physical_size);
  // FNV-1a: stable across runs, good enough to deduplicate files.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint8_t byte : blob) {
    hash = (hash ^ byte) * 0x100000001B3ull;
  }
  std::lock_guard lock(dump_mutex);
  if (dump_directory.empty() || !dumped.insert(hash).second) {
    return;
  }
  auto path = fmt::format("{}/{}_{:016X}.bin", dump_directory, vertex ? "vs" : "ps", hash);
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(blob.data(), 1, blob.size(), file);
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

// D3DDevice_BeginTiling(device, flags, count, rects, clear colour, clear z,
// clear stencil): the game hands it the bands its MSAA scene is split into -
// two of 384 rows at 1280x720 2x - and Direct3D records the scene's commands
// and replays them once per band at EndTiling, predicated on each draw's
// extent. On the host the surfaces are textures of any size, so one band that
// covers them all does the same work once: ~1,500 fewer draws a frame in
// Showdown Town. The game's own state (surfaces, MSAA mode, band rectangles)
// is left alone; only this call sees a single band.
void SingleTile(PPCContext& ctx, uint8_t* base) {
  const uint32_t count = ctx.r5.u32, rects = ctx.r6.u32;
  if (count < 2 || count > 16 || !rects || !REXCVAR_GET(renut_single_tile)) return;
  int32_t box[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* rect = renut::GuestPtr(base, rects + i * 16);
    for (uint32_t k = 0; k < 4; ++k) {
      const int32_t v = int32_t(LoadBE32(rect + k * 4));
      box[k] = k < 2 ? std::min(box[k], v) : std::max(box[k], v);
    }
  }
  static uint32_t scratch = 0;
  if (!scratch) scratch = rex::system::kernel_memory()->SystemHeapAlloc(16, 16);
  if (!scratch) return;
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t v = uint32_t(box[k]);
    const uint8_t bytes[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    std::memcpy(renut::GuestPtr(base, scratch + k * 4), bytes, 4);
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("Renut: predicated tiling {} bands -> one pass ({},{})-({},{})", count, box[0], box[1],
                box[2], box[3]);
  }
  ctx.r5.u32 = 1;
  ctx.r6.u32 = scratch;
  // The game still marks each object for the bands it falls in; with one band
  // every mark has to count.
  SetSingleBinTiling(true);
}

// Called for every intercepted function. Returns true if a handler took it.
inline bool Enter(Function function, PPCContext& ctx, uint8_t* base) {
  renut::native_probe::D3DEnter(function, ctx, base);
  if (function == kD3DDevice_BeginTiling) SingleTile(ctx, base);
  if (int32_t every = ReportEvery()) {
    counts[function].fetch_add(1, std::memory_order_relaxed);
    if (function == kD3DDevice_Swap) {
      static std::atomic<uint32_t> swaps{0};
      if ((++swaps % uint32_t(every)) == 0) {
        Report();
      }
    }
  }
  if (renut::native_d3d::GetMode() != renut::native_d3d::Mode::kOff &&
      renut::native_d3d::OnEnter(function, ctx, base)) {
    return true;
  }
  if (!PlumeActive()) {
    return false;
  }
  Handler handler = handlers[function].load(std::memory_order_acquire);
  return handler && handler(ctx, base);
}

}  // namespace

// Called after the original (or a handler) ran; `r3`/`r4` are the arguments
// it was called with.
inline void Leave(Function function, uint32_t r3, uint32_t r4, PPCContext& ctx, uint8_t* base) {
  if (renut::native_d3d::GetMode() != renut::native_d3d::Mode::kOff) {
    renut::native_d3d::OnLeave(function, r3, ctx, base);
  }
  switch (function) {
    case kXGRegisterVertexShader:
      DumpShaderObject(base, r3, true);
      break;
    case kXGRegisterPixelShader:
      DumpShaderObject(base, r3, false);
      break;
    case kD3DDevice_CreateVertexShader:
      DumpShaderObject(base, ctx.r3.u32, true);
      break;
    case kD3DDevice_CreatePixelShader:
      DumpShaderObject(base, ctx.r3.u32, false);
      break;
    default:
      break;
  }
}

bool PlumeActive() {
  return true;
}

namespace {
std::atomic<bool> single_bin_tiling{false};
}  // namespace

void SetSingleBinTiling(bool single) { single_bin_tiling.store(single, std::memory_order_relaxed); }
bool GetSingleBinTiling() { return single_bin_tiling.load(std::memory_order_relaxed); }

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
  REX_HOOK_RAW(symbol) {                                                           \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;                             \
    if (!renut::d3d::Enter(renut::d3d::k##name, ctx, base)) {                    \
      __imp__##symbol(ctx, base);                                                \
    }                                                                            \
    renut::d3d::Leave(renut::d3d::k##name, r3, r4, ctx, base);                   \
  }
#include "d3d_functions.inc"
#undef X
