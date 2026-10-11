// Native renderer v1: the engine's world draws at the engine.
//
// From the recompiled PowerPC (see the native-renderer-experiment notes): every
// world draw of N&B goes through sub_823A9C38(context, model, record, pass) -
// 3,447 a frame in Showdown Town - which binds the draw record's buffers,
// shaders, textures and constants and calls sub_823AA1A0 to draw. The record:
//   +0  -> {vertex buffer object, stride}     +4  extra streams and constants
//   +8  -> {vertex shader object}             +12 -> {pixel shader object}
//   +16 -> blend factor (pass 3)              +20 constant lists
//   +24 textures                              +28 -> {index buffer object, index count}
// The context's first word is the D3D device.
//
// Each call gets an id, kept with what the record names in a ring, and the id
// goes to the GPU in vertex shader booleans 104..127: the device's boolean
// shadow (device + 10112, a word of 32 per register, the way
// D3DDevice_SetVertexShaderConstantB @ 0x82221930 writes it) and its dirty bit
// (bit 56 of the 64-bit word at device + 32), so the draw that follows flushes
// them into the command buffer ahead of itself. The Plume renderer reads them
// back at the draw (plume_gpu.cpp) and knows which engine draw it has.
#include "native_engine.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <cstring>
#include <mutex>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/ppc/func.h>

#include "native_probe.h"

REXCVAR_DEFINE_BOOL(renut_native_engine_tags, false, "reNut",
                    "Native renderer v1: tag each engine world draw (sub_823A9C38) so that the Plume "
                    "renderer knows which engine draw record it is drawing (needed by renut_plume_native's "
                    "view and surfaces)");

namespace renut::native_engine {
namespace {

constexpr uint32_t kRingSize = 1u << 16;
constexpr uint32_t kTagMask = (1u << (32 - kTagShift)) - 1;

struct Slot {
  std::atomic<uint32_t> tag{0};
  Draw draw;
};
std::array<Slot, kRingSize> g_ring;
std::atomic<uint32_t> g_next{1};

std::atomic<uint64_t> g_tagged{0}, g_seen{0}, g_found{0}, g_count_ok{0}, g_untagged{0}, g_native_view{0};
std::atomic<uint64_t> g_shadow_culled{0}, g_shadow_batches{0}, g_shadow_batched{0}, g_native_lit{0};
thread_local bool t_has_sphere = false;
thread_local float t_sphere[4] = {};
std::array<std::atomic<uint64_t>, 9> g_vb_word{}, g_ib_word{};
std::mutex g_shader_mutex;
std::unordered_map<uint64_t, uint64_t> g_pixel_draws, g_vertex_draws, g_pair_draws;
uint64_t g_colour_draws = 0;
std::mutex g_sample_mutex;
std::string g_samples;
uint32_t g_sample_count = 0;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void Tag(PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(renut_native_engine_tags);
  if (!enabled) return;
  const uint32_t context = ctx.r3.u32, model = ctx.r4.u32, record = ctx.r5.u32;
  if (context < 0x10000u || record < 0x10000u) return;
  const uint32_t device = Load32(base, context);
  if (device < 0x10000u) return;
  uint32_t tag = g_next.fetch_add(1, std::memory_order_relaxed) & kTagMask;
  if (!tag) tag = g_next.fetch_add(1, std::memory_order_relaxed) & kTagMask;
  Slot& slot = g_ring[tag & (kRingSize - 1)];
  slot.tag.store(0, std::memory_order_relaxed);
  Draw& d = slot.draw;
  d = Draw();
  d.context = context;
  d.model = model;
  d.record = record;
  d.pass = ctx.r6.u32;
  if (const uint32_t vb_info = Load32(base, record + 0)) {
    d.vertex_buffer = Load32(base, vb_info);
    d.stride = Load32(base, vb_info + 4);
  }
  if (const uint32_t vs = Load32(base, record + 8)) d.vertex_shader = Load32(base, vs);
  if (const uint32_t ps = Load32(base, record + 12)) d.pixel_shader = Load32(base, ps);
  if (const uint32_t ib_info = Load32(base, record + 28)) {
    d.index_buffer = Load32(base, ib_info);
    d.index_count = Load32(base, ib_info + 4);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    if (d.vertex_buffer >= 0x10000u) d.vb_words[i] = Load32(base, d.vertex_buffer + i * 4);
    if (d.index_buffer >= 0x10000u) d.ib_words[i] = Load32(base, d.index_buffer + i * 4);
  }
  d.has_sphere = t_has_sphere;
  if (t_has_sphere) std::memcpy(d.sphere, t_sphere, sizeof(t_sphere));
  slot.tag.store(tag, std::memory_order_release);
  // Booleans 104..127 = the tag; 96..103 keep whatever the game set.
  const uint32_t word = device + 10112 + 3 * 4;
  Store32(base, word, (Load32(base, word) & 0xFFu) | (tag << kTagShift));
  const uint32_t dirty_hi = Load32(base, device + 32);
  Store32(base, device + 32, dirty_hi | 0x01000000u);  // bit 56 of the big-endian 64-bit word
  g_tagged.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool Lookup(uint32_t tag, Draw& out) {
  if (!tag) return false;
  const Slot& slot = g_ring[tag & (kRingSize - 1)];
  if (slot.tag.load(std::memory_order_acquire) != tag) return false;
  out = slot.draw;
  // Overwritten while copying: the ring wrapped, which takes 65,536 draws.
  return slot.tag.load(std::memory_order_acquire) == tag;
}

void NoteDraw(bool found, bool count_matches, int vb_word, int ib_word, bool native_view) {
  g_seen.fetch_add(1, std::memory_order_relaxed);
  if (!found) return;
  g_found.fetch_add(1, std::memory_order_relaxed);
  if (count_matches) g_count_ok.fetch_add(1, std::memory_order_relaxed);
  g_vb_word[vb_word < 0 ? 8 : vb_word].fetch_add(1, std::memory_order_relaxed);
  g_ib_word[ib_word < 0 ? 8 : ib_word].fetch_add(1, std::memory_order_relaxed);
  if (native_view) g_native_view.fetch_add(1, std::memory_order_relaxed);
}

void NoteUntagged() { g_untagged.fetch_add(1, std::memory_order_relaxed); }

void NoteShaders(uint64_t vertex_shader, uint64_t pixel_shader) {
  std::lock_guard lock(g_shader_mutex);
  ++g_pixel_draws[pixel_shader];
  ++g_vertex_draws[vertex_shader];
  ++g_pair_draws[vertex_shader * 1099511628211ull ^ pixel_shader];
  ++g_colour_draws;
}

void SetObject(uint32_t object, const uint8_t* base) {
  t_has_sphere = false;
  if (object < 0x10000u) return;
  t_sphere[0] = LoadFloat(base, object + 16);
  t_sphere[1] = LoadFloat(base, object + 20);
  t_sphere[2] = LoadFloat(base, object + 24);
  t_sphere[3] = LoadFloat(base, object + 32);
  t_has_sphere = t_sphere[3] > 0.f && t_sphere[3] < 1e6f;
}

void NoteShadowCull() { g_shadow_culled.fetch_add(1, std::memory_order_relaxed); }

void NoteNativeLit() { g_native_lit.fetch_add(1, std::memory_order_relaxed); }

void NoteShadowBatch(uint32_t instances) {
  g_shadow_batches.fetch_add(1, std::memory_order_relaxed);
  g_shadow_batched.fetch_add(instances, std::memory_order_relaxed);
}

void AddSample(const std::string& line) {
  std::lock_guard lock(g_sample_mutex);
  if (g_sample_count >= 24) return;
  ++g_sample_count;
  g_samples += line;
}

void Report(std::string& out, uint64_t frames) {
  const double f = double(frames ? frames : 1);
  out += fmt::format(
      "== Engine draw tags (per frame): tagged at the engine {:.1f}; at the Plume draw: tagged {:.1f}, "
      "resolved {:.1f}, index count matches {:.1f}, untagged {:.1f}, drawn by the native view {:.1f}\n",
      g_tagged.exchange(0) / f, g_seen.exchange(0) / f, g_found.exchange(0) / f, g_count_ok.exchange(0) / f,
      g_untagged.exchange(0) / f, g_native_view.exchange(0) / f);
  out += fmt::format(
      "== Shadow maps (per frame): casters under the texel threshold dropped {:.1f}; instanced batches {:.1f} "
      "holding {:.1f} draws\n== Native lit material: {:.1f} draws/frame\n",
      g_shadow_culled.exchange(0) / f, g_shadow_batches.exchange(0) / f, g_shadow_batched.exchange(0) / f,
      g_native_lit.exchange(0) / f);
  {
    std::lock_guard lock(g_shader_mutex);
    auto top = [&](std::unordered_map<uint64_t, uint64_t>& map, const char* what, size_t limit) {
      std::vector<std::pair<uint64_t, uint64_t>> sorted(map.begin(), map.end());
      std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
      out += fmt::format("== Engine colour draws by {} ({} distinct, {:.1f} draws/frame in all)\n", what, sorted.size(),
                         g_colour_draws / f);
      uint64_t running = 0;
      for (size_t i = 0; i < std::min(limit, sorted.size()); ++i) {
        running += sorted[i].second;
        out += fmt::format("   {:016X} {:8.1f}/f  cumulative {:5.1f}%\n", sorted[i].first, sorted[i].second / f,
                           g_colour_draws ? 100.0 * double(running) / double(g_colour_draws) : 0.0);
      }
      map.clear();
    };
    top(g_pixel_draws, "pixel shader", 60);
    top(g_vertex_draws, "vertex shader", 30);
    g_pair_draws.clear();
    g_colour_draws = 0;
  }
  out += "   vertex buffer address decoded from the engine object = the PM4 stream 0 (by word 0..7, none):";
  for (auto& w : g_vb_word) out += fmt::format(" {}", w.exchange(0));
  out += "\n   index buffer address decoded from the engine object = the PM4 indices (by word 0..7, none):";
  for (auto& w : g_ib_word) out += fmt::format(" {}", w.exchange(0));
  out += "\n";
  std::lock_guard lock(g_sample_mutex);
  out += g_samples;
  out += "\n";
}

}  // namespace renut::native_engine

extern "C" void __imp__sub_823A9C38(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_823A9C38) {
  renut::native_probe::EngineEnter(renut::native_probe::kEngine_sub_823A9C38, ctx, base);
  renut::native_engine::Tag(ctx, base);
  __imp__sub_823A9C38(ctx, base);
}
