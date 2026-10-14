// Native renderer reconnaissance (branch native-renderer-experiment).
//
// One run of the game answers where the engine's draws come from, so that the
// native renderer can take them at the engine instead of at the PM4:
//  - every Direct3D device call counted per frame;
//  - for the draw, command-buffer and state calls, the guest call chain that
//    reached them (the return address and four more frames up the guest stack),
//    counted, with the first calls' registers and the objects they point at;
//  - command buffers: how many are recorded and run a frame, how many draws
//    each holds, and how old a buffer is when it runs (recorded every frame, or
//    once and replayed);
//  - the engine functions already read in the recompiled PowerPC (the
//    instanced draw sub_823AA1A0 and its callers, the effect passes
//    sub_823B0CB0 and siblings), counted with their arguments dumped.
//
// Reports go to <files>/native-probe-<n>.txt every 600 swaps (eight kept,
// rotating), whatever the log level. On with renut_native_probe = true (it costs the
// render jobs a lock and a stack walk on every Direct3D call).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/hook.h>

#include "d3d_layer.h"
#include "native_engine.h"
#include "native_probe.h"
#include "guest_address.h"

// The Plume renderer's native mode (plume_gpu.cpp): 0 off, 1 rigid depth
// draws, 2 also the native view.
extern std::atomic<uint32_t> g_renut_native_mode;

REXCVAR_DEFINE_BOOL(renut_native_probe, false, "reNut",
                    "Native renderer reconnaissance: count the engine's draw paths and dump their "
                    "objects to <files>/native-probe-<n>.txt every 600 swaps");

namespace renut::native_probe {


namespace {

using renut::d3d::Function;

constexpr const char* kD3DNames[] = {
#define X(symbol, name) #name,
#include "d3d_functions.inc"
#undef X
};
constexpr const char* kEngineNames[] = {
#define X(symbol) #symbol,
    RENUT_PROBE_ENGINE(X) RENUT_PROBE_ENGINE_EXTERNAL(X)
#undef X
};
// Chains are keyed as (source, ...): sources 0..kFunctionCount-1 are the D3D
// functions, the engine functions follow.
constexpr uint32_t kEngineSource = renut::d3d::kFunctionCount;
constexpr uint32_t kSwapsPerReport = 600;
constexpr int kDepth = 5;
constexpr uint32_t kSamplesPerChain = 2;
constexpr size_t kMaxSampleBytes = 600u << 10;

std::atomic<int> g_enabled{-1};
std::string g_dir;
std::mutex g_mutex;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, address), 4);
  return __builtin_bswap32(v);
}

bool Readable(uint32_t address, uint32_t size) {
  if (address < 0x10000u || uint64_t(address) + size > 0xFFFFF000ull) return false;
  auto* memory = rex::system::kernel_memory();
  if (!memory) return false;
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  return heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

// The guest stack region of this thread, looked up once (and again if the
// stack pointer leaves it), so that walking the back chain never reads
// outside it.
struct StackRange {
  uint32_t lo = 0, hi = 0;
};
thread_local StackRange t_stack;

bool InStack(uint32_t address, uint32_t size) {
  return address >= t_stack.lo && uint64_t(address) + size <= t_stack.hi;
}

void Walk(const PPCContext& ctx, const uint8_t* base, uint32_t* ret) {
  std::fill(ret, ret + kDepth, 0u);
  ret[0] = uint32_t(ctx.lr);
  uint32_t sp = ctx.r1.u32;
  if (!InStack(sp, 8)) {
    auto* memory = rex::system::kernel_memory();
    auto* heap = memory ? memory->LookupHeap(sp) : nullptr;
    uint32_t region = sp, size = 0;
    if (!heap || !heap->QueryBaseAndSize(&region, &size) || !size) return;
    t_stack = {region, uint32_t(std::min<uint64_t>(uint64_t(region) + size, 0xFFFFFFFFull))};
    if (!InStack(sp, 8)) return;
  }
  for (int i = 1; i < kDepth; ++i) {
    const uint32_t caller_sp = Load32(base, sp);
    if (caller_sp <= sp || !InStack(caller_sp - 8, 8)) return;
    const uint32_t r = Load32(base, caller_sp - 8);
    if (r < 0x82000000u || r >= 0x83000000u) return;
    ret[i] = r;
    sp = caller_sp;
  }
}

struct ChainKey {
  uint32_t source;
  uint32_t ret[kDepth];
  bool operator==(const ChainKey& o) const {
    return source == o.source && std::memcmp(ret, o.ret, sizeof(ret)) == 0;
  }
};
struct ChainHash {
  size_t operator()(const ChainKey& k) const {
    uint64_t h = 0xCBF29CE484222325ull ^ k.source;
    for (uint32_t r : k.ret) h = (h ^ r) * 0x100000001B3ull;
    return size_t(h);
  }
};
struct ChainStats {
  uint64_t count = 0, recorded = 0;
  uint32_t samples = 0;
  std::string text;
};

struct CommandBuffer {
  uint64_t begins = 0, runs = 0;
  uint64_t last_begin = UINT64_MAX;
  uint32_t recording_draws = 0, last_draws = 0;
  uint32_t first_chain = 0;  // return address of its first BeginCommandBuffer
};

struct Window {
  uint64_t start_swap = 0;
  std::array<uint64_t, renut::d3d::kFunctionCount> d3d{};
  std::array<uint64_t, kEngineCount> engine{};
  std::array<std::unordered_set<uint32_t>, kEngineCount> engine_r3;
  uint64_t draws_recorded = 0, draws_immediate = 0;
  uint64_t begins = 0, ends = 0, runs = 0, replayed_draws = 0;
  uint64_t age[5] = {};  // run of a buffer recorded: this frame, last, 2-10, older, never seen
  std::unordered_set<uint32_t> run_buffers;
  std::unordered_map<ChainKey, ChainStats, ChainHash> chains;
  size_t sample_bytes = 0;
  // Frames and seconds spent in each native renderer mode (0 off, 1 rigid
  // depth draws, 2 also the native view), for comparing them in one window.
  // Index: native mode | 4 shadow texel cull | 8 shadow instancing | 16 native lit material.
  std::array<uint64_t, 32> mode_frames{};
  std::array<double, 32> mode_seconds{};
};

uint64_t g_swaps = 0;
uint32_t g_report = 0;
Window g_window;
std::unordered_map<uint32_t, CommandBuffer> g_buffers;
std::unordered_set<uint32_t> g_dumped;  // objects dumped already (each once a run)
thread_local uint32_t t_recording = 0;   // the command buffer this thread records, 0 = none

bool Enabled() {
  int e = g_enabled.load(std::memory_order_relaxed);
  if (e < 0) {
    e = REXCVAR_GET(renut_native_probe) ? 1 : 0;
    g_enabled.store(e, std::memory_order_relaxed);
  }
  return e != 0;
}

// What the chain report shows of one call: the argument and non-volatile
// registers, then 128 bytes of each guest object they point at (each object
// once a run).
void Sample(ChainStats& stats, const PPCContext& ctx, const uint8_t* base) {
  if (stats.samples >= kSamplesPerChain || g_window.sample_bytes > kMaxSampleBytes) return;
  ++stats.samples;
  std::string& t = stats.text;
  t += fmt::format("    sample {} (swap {}):\n     ", stats.samples, g_swaps);
  const PPCRegister* regs[] = {&ctx.r3,  &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,  &ctx.r8,  &ctx.r9,  &ctx.r10,
                               &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  const int numbers[] = {3, 4, 5, 6, 7, 8, 9, 10, 24, 25, 26, 27, 28, 29, 30, 31};
  for (int i = 0; i < 16; ++i) t += fmt::format(" r{}={:08X}", numbers[i], regs[i]->u32);
  t += fmt::format(" f1={} f2={}\n", ctx.f1.f64, ctx.f2.f64);
  for (int i = 0; i < 16; ++i) {
    const uint32_t a = regs[i]->u32;
    if (a < 0x30000000u || (a & 3) || !Readable(a, 128) || !g_dumped.insert(a).second) continue;
    t += fmt::format("      r{} -> {:08X}:", numbers[i], a);
    for (uint32_t w = 0; w < 32; ++w) t += fmt::format("{}{:08X}", w % 8 ? " " : "\n        ", Load32(base, a + w * 4));
    t += "\n";
  }
  g_window.sample_bytes += t.size();
}

void Chain(uint32_t source, const PPCContext& ctx, const uint8_t* base, bool sample) {
  ChainKey key{source, {}};
  Walk(ctx, base, key.ret);
  ChainStats& stats = g_window.chains[key];
  ++stats.count;
  if (t_recording) ++stats.recorded;
  if (sample) Sample(stats, ctx, base);
}

std::string Name(uint32_t source) {
  return source < kEngineSource ? kD3DNames[source] : kEngineNames[source - kEngineSource];
}

void WriteReport() {
  Window& w = g_window;
  const uint64_t frames = std::max<uint64_t>(1, g_swaps - w.start_swap);
  std::string out = fmt::format(
      "native-probe report {} - swaps {}..{} ({} frames)\n\n"
      "== Draws per frame: recorded into command buffers {:.1f}, immediate {:.1f}; "
      "draws replayed from run buffers {:.1f}\n"
      "== Command buffers per frame: begin {:.1f}, end {:.1f}, run {:.1f}, distinct run in window {}\n"
      "   age of a run buffer: recorded this frame {}, last frame {}, 2-10 frames {}, older {}, never seen {}\n\n",
      g_report, w.start_swap, g_swaps, frames, double(w.draws_recorded) / frames,
      double(w.draws_immediate) / frames, double(w.replayed_draws) / frames, double(w.begins) / frames,
      double(w.ends) / frames, double(w.runs) / frames, w.run_buffers.size(), w.age[0], w.age[1], w.age[2], w.age[3],
      w.age[4]);
  out += "== Frame rate by native renderer mode (plume-native, +4 shadow texel cull, +8 shadow instancing, "
         "+16 native lit material):";
  for (uint32_t m = 0; m < w.mode_frames.size(); ++m) {
    if (w.mode_frames[m]) {
      out += fmt::format(" mode {} {:.2f} FPS ({} frames, {:.1f} s);", m, w.mode_frames[m] / w.mode_seconds[m],
                         w.mode_frames[m], w.mode_seconds[m]);
    }
  }
  out += "\n";
  renut::native_engine::Report(out, frames);
  out += "== D3D device calls per frame\n";
  std::vector<std::pair<uint64_t, uint32_t>> calls;
  for (uint32_t i = 0; i < renut::d3d::kFunctionCount; ++i) {
    if (w.d3d[i]) calls.emplace_back(w.d3d[i], i);
  }
  std::sort(calls.rbegin(), calls.rend());
  for (auto& [n, i] : calls) out += fmt::format("  {:10.1f} {}\n", double(n) / frames, kD3DNames[i]);
  out += "\n== Engine functions per frame (distinct r3 in window)\n";
  for (uint32_t i = 0; i < kEngineCount; ++i) {
    out += fmt::format("  {:10.1f} {} ({})\n", double(w.engine[i]) / frames, kEngineNames[i], w.engine_r3[i].size());
  }
  out += "\n== Command buffers seen (first begin's caller, begins, runs, draws in last recording)\n";
  std::vector<std::pair<uint32_t, const CommandBuffer*>> buffers;
  for (auto& [address, cb] : g_buffers) buffers.emplace_back(address, &cb);
  std::sort(buffers.begin(), buffers.end(),
            [](auto& a, auto& b) { return a.second->runs + a.second->begins > b.second->runs + b.second->begins; });
  for (size_t i = 0; i < std::min<size_t>(buffers.size(), 80); ++i) {
    const CommandBuffer& cb = *buffers[i].second;
    out += fmt::format("  cb {:08X} from {:08X}: begins {} runs {} draws {} last begin swap {}\n", buffers[i].first,
                       cb.first_chain, cb.begins, cb.runs, cb.last_draws,
                       cb.last_begin == UINT64_MAX ? 0 : cb.last_begin);
  }
  out += "\n== Call chains (per frame, recorded share; return addresses innermost first)\n";
  std::vector<std::pair<const ChainKey*, const ChainStats*>> chains;
  for (auto& [key, stats] : w.chains) chains.emplace_back(&key, &stats);
  std::sort(chains.begin(), chains.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
  for (size_t i = 0; i < std::min<size_t>(chains.size(), 400); ++i) {
    const ChainKey& k = *chains[i].first;
    const ChainStats& s = *chains[i].second;
    out += fmt::format("  {:9.2f}/f rec {:3.0f}% {:<40}", double(s.count) / frames,
                       s.count ? 100.0 * double(s.recorded) / double(s.count) : 0.0, Name(k.source));
    for (uint32_t r : k.ret) out += fmt::format(" {:08X}", r);
    out += "\n" + s.text;
  }
  if (!g_dir.empty()) {
    const std::string path = fmt::format("{}/native-probe-{}.txt", g_dir, g_report % 8);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(out.data(), 1, out.size(), f);
      std::fclose(f);
    }
  }
  REXLOG_INFO("Renut: native probe report {} written ({} chains)", g_report, w.chains.size());
  ++g_report;
  g_window = Window();
  g_window.start_swap = g_swaps;
}

bool IsDraw(uint32_t f) {
  switch (f) {
    case d3d::kD3DDevice_DrawIndexedVertices:
    case d3d::kD3DDevice_DrawVertices:
    case d3d::kD3DDevice_DrawIndexedTessellatedVertices:
    case d3d::kD3DDevice_BeginVertices:
    case d3d::kD3DDevice_BeginIndexedVertices:
    case d3d::kD3DDevice_DrawVerticesUP:
    case d3d::kD3DDevice_DrawIndexedVerticesUP:
      return true;
    default:
      return false;
  }
}

// The calls whose chains are kept; the draws and the buffer calls get samples.
bool Chained(uint32_t f) {
  switch (f) {
    case d3d::kD3DDevice_BeginCommandBuffer:
    case d3d::kD3DDevice_EndCommandBuffer:
    case d3d::kD3DDevice_RunCommandBuffer:
    case d3d::kD3DDevice_SetVertexShader:
    case d3d::kD3DDevice_SetPixelShader:
    case d3d::kD3DDevice_SetStreamSource:
    case d3d::kD3DDevice_SetIndices:
    case d3d::kD3DDevice_SetVertexDeclaration:
    case d3d::kD3DDevice_SetVertexShaderConstantFN:
    case d3d::kD3DDevice_SetTexture:
    case d3d::kD3D_CDevice_QueueIndirectBuffers:
    case d3d::kD3D_CDevice_AddCommandsToPrimaryBuffer:
      return true;
    default:
      return IsDraw(f);
  }
}

// Calls made tens of millions of times a second while the game waits on the
// GPU: not counted, they would only cost time.
bool Polling(uint32_t f) {
  const char* name = kD3DNames[f];
  return std::strstr(name, "Blocker") || std::strstr(name, "BlockOn") || std::strstr(name, "IsFencePending") ||
         std::strstr(name, "BeginRingAlloc");
}

}  // namespace

void SetFilesDirectory(std::string directory) {
  std::lock_guard lock(g_mutex);
  g_dir = std::move(directory);
}

void D3DEnter(uint32_t f, PPCContext& ctx, uint8_t* base) {
  static const std::array<bool, renut::d3d::kFunctionCount> polling = [] {
    std::array<bool, renut::d3d::kFunctionCount> p{};
    for (uint32_t i = 0; i < renut::d3d::kFunctionCount; ++i) p[i] = Polling(i);
    return p;
  }();
  if (polling[f] || !Enabled()) return;
  std::lock_guard lock(g_mutex);
  ++g_window.d3d[f];
  if (f == d3d::kD3DDevice_Swap) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    const uint32_t mode = std::min<uint32_t>(g_renut_native_mode.load(std::memory_order_relaxed), 31);
    ++g_window.mode_frames[mode];
    g_window.mode_seconds[mode] += seconds;
    if (++g_swaps - g_window.start_swap >= kSwapsPerReport) WriteReport();
    return;
  }
  if (f == d3d::kD3DDevice_BeginCommandBuffer) {
    const uint32_t cb = ctx.r4.u32;
    CommandBuffer& b = g_buffers[cb];
    if (!b.first_chain) b.first_chain = uint32_t(ctx.lr);
    ++b.begins;
    b.last_begin = g_swaps;
    b.recording_draws = 0;
    t_recording = cb ? cb : 1;
    ++g_window.begins;
  } else if (f == d3d::kD3DDevice_EndCommandBuffer) {
    if (t_recording > 1) {
      CommandBuffer& b = g_buffers[t_recording];
      b.last_draws = b.recording_draws;
    }
    t_recording = 0;
    ++g_window.ends;
  } else if (f == d3d::kD3DDevice_RunCommandBuffer) {
    const uint32_t cb = ctx.r4.u32;
    auto found = g_buffers.find(cb);
    int bucket = 4;
    if (found != g_buffers.end() && found->second.last_begin != UINT64_MAX) {
      const uint64_t age = g_swaps - found->second.last_begin;
      bucket = age == 0 ? 0 : age == 1 ? 1 : age <= 10 ? 2 : 3;
      ++found->second.runs;
      g_window.replayed_draws += found->second.last_draws;
    } else {
      ++g_buffers[cb].runs;
    }
    ++g_window.age[bucket];
    ++g_window.runs;
    g_window.run_buffers.insert(cb);
  } else if (IsDraw(f)) {
    if (t_recording) {
      ++g_window.draws_recorded;
      if (t_recording > 1) ++g_buffers[t_recording].recording_draws;
    } else {
      ++g_window.draws_immediate;
    }
  }
  if (Chained(f)) {
    const bool sample = IsDraw(f) || f == d3d::kD3DDevice_BeginCommandBuffer || f == d3d::kD3DDevice_RunCommandBuffer;
    Chain(f, ctx, base, sample);
  }
}

void EngineEnter(uint32_t which, PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return;
  std::lock_guard lock(g_mutex);
  ++g_window.engine[which];
  if (g_window.engine_r3[which].size() < 4096) g_window.engine_r3[which].insert(ctx.r3.u32);
  Chain(kEngineSource + which, ctx, base, true);
}

}  // namespace renut::native_probe

// Strong overrides of the weak recompiled engine functions: count, then run
// the original.
#define X(symbol)                                                                      \
  extern "C" void __imp__##symbol(PPCContext& __restrict, uint8_t*);                   \
  REX_HOOK_RAW(symbol) {                                                                 \
    renut::native_probe::EngineEnter(renut::native_probe::kEngine_##symbol, ctx, base); \
    __imp__##symbol(ctx, base);                                                        \
  }
RENUT_PROBE_ENGINE(X)
#undef X
