// Native Direct3D front end (see native_d3d.h).
//
// Device layout, from the recompiled Direct3D (names from the symbol map of
// etonedemid/reNut_linuxfixes): the device keeps every GPU register the game
// sets in shadow blocks, and five 64-bit dirty masks say which parts a draw
// still has to send. The draw functions (sub_826567C8 DrawIndexedVertices,
// sub_8222C7A0 DrawVertices, sub_8222CC50 BeginVertices, ...) all begin with
// the same flush, which is Flush() below:
//
//   dirty +0   vertex ALU constants  shadow +1920, 4 constants a bit -> 0x4000
//   dirty +8   pixel ALU constants   shadow +6016                     -> 0x4400
//   dirty +16  bits 17-20 shaders (SetPending_Shaders, sub_8222BF10)
//              bits 0-11  0x2200.. shadow +10548   bits 12-16 0x2180.. +10528
//              bits 42-57 0x2000.. shadow +10368   bits 21-41 0x2100.. +10444
//   dirty +24  bits 0-31  fetch constants, 6 dwords a slot, shadow +1152 -> 0x4800
//              bits 34-54 0x2280.. shadow +10596
//   dirty +32  bits 0-37  0x2300.. shadow +10680   bits 38-45 0x2380.. +10832
//              bits 49-54 clip planes (sub_8220E2B0) 0x2388.. shadow +10272
//              bit 56     booleans and loops 0x4900-0x4927, shadow +10112
//   +40 a mask of the dirty +16 bits that need SetPending_Predicated
//   +48/+56 the ring's write pointer and end (pre-increment stores)
//
// A bit numbered from the top of a mask names the first register of a block,
// the next bit the next register, and so on (SetPending_RenderStates,
// sub_8222CB90).
#include "native_d3d.h"

#include "d3d_layer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(renut_native_d3d, 0, "reNut",
                     "Native Direct3D front end: 0 off, 1 verify (built next to the PM4 and checked "
                     "draw by draw against it, files/native-d3d-verify.txt), 2 native (renders without "
                     "PM4 draws)");

// Guest functions the front end calls, as the Direct3D it replaces does.
extern "C" {
REX_FUNC(sub_822127F8);  // vertex shader binding check / rebind to the declaration
REX_FUNC(sub_82656DA0);  // D3D_PatchVertexShaderToMatchPixelShader
REX_FUNC(sub_821E5218);  // D3D_PatchVertexShaderToMatchVertexDeclaration
REX_FUNC(sub_82210AB0);  // KickOff: more ring space
}

namespace renut::native_d3d {
namespace {

using renut::d3d::Function;

// ---------------------------------------------------------------------------
// Guest memory (big-endian; on Android base + address is the mapping).

inline uint8_t U8(const uint8_t* base, uint32_t a) { return base[a]; }
inline uint16_t U16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}
inline uint32_t U32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline uint64_t U64(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, base + a, 8);
  return __builtin_bswap64(v);
}
inline void S8(uint8_t* base, uint32_t a, uint8_t v) { base[a] = v; }
inline void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
inline void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(base + a, &v, 8);
}

// A guest pointer as the GPU addresses it: Direct3D's own conversion,
// (a & 0x1FFFFFFF) plus 0x1000 for the 0xE0000000 view.
inline uint32_t Phys(uint32_t a) { return (a & 0x1FFFFFFF) + (((a >> 20) + 0x200) & 0x1000); }
// Physical memory as the CPU sees it, through the 0xA0000000 view.
inline uint32_t Cpu(uint32_t physical) { return physical | 0xA0000000u; }

// Device fields.
constexpr uint32_t kPredicatedMask = 40, kRingWrite = 48, kRingEnd = 56;
constexpr uint32_t kFetchShadow = 1152, kVsConstants = 1920, kPsConstants = 6016;
constexpr uint32_t kBoolLoop = 10112, kClipPlanes = 10272;
constexpr uint32_t kRegs2000 = 10368, kRegs2100 = 10444, kRegs2180 = 10528, kRegs2200 = 10548;
constexpr uint32_t kRegs2280 = 10596, kRegs2300 = 10680, kRegs2380 = 10832;
constexpr uint32_t kVertexShader = 12696, kPixelShader = 12692, kDeclaration = 11820;
constexpr uint32_t kIndexBuffer = 12436, kRecording = 13516;
// Draw primitive splitting table (Direct3D's, 8 bytes a primitive type: the
// divisor and the overlap of a strip continued in a second draw).
constexpr uint32_t kPrimitiveSplit = 0x82004EB8;

std::atomic<int32_t> g_mode{-1};

// Every native draw's serial. In verify mode it also goes into the PM4, in a
// NOP just ahead of the draw's packets, so the command processor finds the
// native draw each PM4 draw is.
std::atomic<uint32_t> g_draw_serial{0};
constexpr uint32_t kTagPayload = 0xFF000000u;

// Calls per Direct3D function, for the report.
std::array<std::atomic<uint64_t>, renut::d3d::kFunctionCount> g_calls{};
constexpr const char* kFunctionNames[] = {
#define X(symbol, name) #name,
#include "d3d_functions.inc"
#undef X
};

// ---------------------------------------------------------------------------
// Counters for the report.

struct Counters {
  std::atomic<uint64_t> draws{0}, flushes{0}, shader_loads{0}, patched_loads{0}, literal_blocks{0};
  std::atomic<uint64_t> screen_extent{0}, predicated_tiling{0}, set_pending_predicated{0};
  std::atomic<uint64_t> begin_vertices{0}, markers{0}, runs{0}, recordings{0}, publishes{0};
  std::atomic<uint64_t> no_scratch{0}, hiz_events{0}, unknown_runs{0}, nested_recordings{0};
  std::atomic<uint64_t> surface_clips{0}, tiled_surface_clips{0}, foreign_immediate{0}, devices{0};
} g_counts;

// ---------------------------------------------------------------------------
// Lists.

class Builder {
 public:
  explicit Builder(List& list) : list_(list) {}
  void Regs(uint32_t first, const uint32_t* values, uint32_t count) {
    if (!count) return;
    list_.words.push_back(kOpRegs | (count << 8));
    list_.words.push_back(first);
    list_.words.insert(list_.words.end(), values, values + count);
  }
  void Reg(uint32_t index, uint32_t value) { Regs(index, &value, 1); }
  // `count` big-endian dwords of guest memory, read now.
  void RegsFromGuest(const uint8_t* base, uint32_t first, uint32_t address, uint32_t count) {
    if (!count) return;
    list_.words.push_back(kOpRegs | (count << 8));
    list_.words.push_back(first);
    for (uint32_t i = 0; i < count; ++i) list_.words.push_back(U32(base, address + i * 4));
  }
  void Shader(uint32_t type, uint32_t physical, uint32_t dwords, uint64_t hash) {
    list_.words.push_back(kOpShader);
    list_.words.push_back((physical & ~3u) | (type & 3));
    list_.words.push_back(dwords);
    list_.words.push_back(uint32_t(hash));
    list_.words.push_back(uint32_t(hash >> 32));
  }
  size_t Draw(uint32_t flags, uint32_t viz, uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
              uint32_t serial) {
    const size_t at = list_.words.size();
    list_.words.push_back(kOpDraw | (flags << 8));
    list_.words.push_back(viz);
    list_.words.push_back(initiator);
    list_.words.push_back(dma_base);
    list_.words.push_back(dma_size);
    list_.words.push_back(serial);
    ++list_.draws;
    return at;
  }
  void Bin(uint32_t which, uint32_t value) {
    list_.words.push_back(kOpBin | (which << 8));
    list_.words.push_back(value);
  }
  void Run(std::shared_ptr<const List> child) {
    list_.words.push_back(kOpRun);
    list_.words.push_back(uint32_t(list_.runs.size()));
    list_.runs.push_back(std::move(child));
  }
  void Marker(uint32_t kind, uint32_t serial) {
    list_.words.push_back(kOpMarker | (kind << 8));
    list_.words.push_back(serial);
  }
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height) {
    list_.words.push_back(kOpSwap);
    list_.words.push_back(frontbuffer);
    list_.words.push_back(width);
    list_.words.push_back(height);
  }
  List& list() { return list_; }

 private:
  List& list_;
};

// The front end's state. Direct3D is driven from one thread at a time (the
// device's thread ownership); the lock keeps the kick-off publishing honest.
struct FrontEnd {
  std::recursive_mutex mutex;
  std::shared_ptr<List> immediate = std::make_shared<List>();
  // The command buffer each device is recording. The game's render jobs
  // record on devices of their own (the job's context +68), several at once
  // on different threads; the immediate list is the swapping device's.
  std::unordered_map<uint32_t, std::shared_ptr<List>> recording;
  uint32_t main_device = 0;
  std::unordered_map<uint32_t, std::shared_ptr<const List>> command_buffers;
  std::deque<std::shared_ptr<const List>> published;
  std::mutex published_mutex;
  uint32_t marker_serial = 0;
} g_front;

List& Current(uint32_t dev) {
  auto it = g_front.recording.find(dev);
  if (it != g_front.recording.end() && it->second) return *it->second;
  if (g_front.main_device && dev != g_front.main_device) {
    g_counts.foreign_immediate.fetch_add(1, std::memory_order_relaxed);
  }
  return *g_front.immediate;
}

void Publish() {
  if (g_front.immediate->words.empty()) return;
  std::shared_ptr<const List> done = std::move(g_front.immediate);
  g_front.immediate = std::make_shared<List>();
  {
    std::lock_guard lock(g_front.published_mutex);
    g_front.published.push_back(std::move(done));
  }
  g_counts.publishes.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Scratch guest memory for what Direct3D would put in the ring: vertex data
// of BeginVertices, and vertex shaders patched for this draw's declaration.
// It is read by the command processor when it runs the list, so it is reused
// only after it went round: 32 MB, a few dozen frames.

struct Scratch {
  uint32_t base = 0, size = 0, offset = 0;
} g_scratch;

uint32_t ScratchAlloc(uint32_t bytes) {
  if (!g_scratch.base) {
    auto* memory = rex::system::kernel_memory();
    g_scratch.size = 32u << 20;
    g_scratch.base =
        memory ? memory->SystemHeapAlloc(g_scratch.size, 4096, rex::memory::kSystemHeapPhysical) : 0;
    if (!g_scratch.base) {
      g_counts.no_scratch.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    REXLOG_INFO("Renut: native Direct3D scratch {:08X} ({} MB)", g_scratch.base, g_scratch.size >> 20);
  }
  bytes = (bytes + 63) & ~63u;
  if (bytes > g_scratch.size / 4) return 0;
  if (g_scratch.offset + bytes > g_scratch.size) g_scratch.offset = 0;
  const uint32_t address = g_scratch.base + g_scratch.offset;
  g_scratch.offset += bytes;
  return address;
}

// A guest dword standing in for SetPending_Shaders' stack slot that
// D3D_PatchVertexShaderToMatchPixelShader updates (SQ_PROGRAM_CNTL).
uint32_t ProgramSlot() {
  static uint32_t slot = 0;
  if (!slot) {
    if (auto* memory = rex::system::kernel_memory()) slot = memory->SystemHeapAlloc(16, 16);
  }
  return slot;
}

// ---------------------------------------------------------------------------
// The flush, from the PowerPC.

struct Device {
  uint8_t* base;
  uint32_t dev;
  PPCContext& ctx;
  Builder out;
  bool verify;
};

// SetPending_RenderStates (sub_8222CB90): bit i from the top -> register
// first + i, value shadow[i].
void RenderStates(Device& d, uint64_t mask, uint32_t first, uint32_t shadow) {
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    first += skip;
    shadow += skip * 4;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, first, shadow, run);
    first += run;
    shadow += run * 4;
    mask = run >= 64 ? 0 : mask << run;
  }
}

// SetPending_AluConstants (sub_8222D0E8): four constants (16 registers) a bit.
void AluConstants(Device& d, uint64_t mask, uint32_t first, uint32_t shadow) {
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    first += skip * 16;
    shadow += skip * 64;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, first, shadow, run * 16);
    first += run * 16;
    shadow += run * 64;
    mask = run >= 64 ? 0 : mask << run;
  }
}

// SetPending_FetchConstants (sub_8222AB88): six registers a slot from +1152.
// (It then invalidates the texture cache, registers 0x5000-0x5002: nothing
// to do on the host.)
void FetchConstants(Device& d, uint64_t mask) {
  uint32_t slot = 0;
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    slot += skip;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, 0x4800 + slot * 6, d.dev + kFetchShadow + slot * 24, run * 6);
    slot += run;
    mask = run >= 64 ? 0 : mask << run;
  }
}

// SetPending_ClipPlanes (sub_8220E2B0): register 0x2007 again, a coherency
// wait (host: nothing), then four registers a plane from 0x2388.
void ClipPlanes(Device& d, uint64_t mask) {
  d.out.Reg(0x2007, U32(d.base, d.dev + kRegs2000 + 7 * 4));
  uint32_t plane = 0;
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    plane += skip;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, 0x2388 + plane * 4, d.dev + kClipPlanes + plane * 16, run * 4);
    plane += run;
    mask = run >= 64 ? 0 : mask << run;
  }
}

// SetLiteralShaderConstants (sub_8222BDC8): the constants a shader defines in
// its microcode header, loaded from its physical part straight into the
// constant registers (LOAD_ALU_CONSTANT) - never into the device's shadow.
// Entries {u16 constant, u16 dwords, u32 offset} from header + header[20].
void LiteralConstants(Device& d, uint32_t header, uint32_t physical) {
  const uint32_t table = U32(d.base, header + 20);
  if (!table) return;
  const uint32_t t = header + table;
  uint32_t entry = t + 20;
  const uint32_t end = U32(d.base, t + 16) + entry;
  while (entry < end) {
    const uint32_t constant = U16(d.base, entry);
    const uint32_t dwords = U16(d.base, entry + 2);
    if (!dwords) break;
    const uint32_t source = U32(d.base, entry + 4) + physical;
    entry += 8;
    d.out.RegsFromGuest(d.base, 0x4000 + constant * 4, Cpu(Phys(source)), dwords);
    g_counts.literal_blocks.fetch_add(1, std::memory_order_relaxed);
  }
}

uint64_t Hash(Device& d, uint32_t physical, uint32_t dwords) {
  if (!d.verify) return 0;
  return XXH3_64bits(d.base + Cpu(physical), size_t(dwords) * 4);
}

// sub_822127F8(device, shader, declaration, binding): whether vertex shader
// binding 0 or 1 matches the declaration (rebinding it in place when the GPU
// is done with it). Called as Direct3D calls it.
bool BindingMatches(Device& d, uint32_t shader, uint32_t declaration, uint32_t binding) {
  d.ctx.r3.u64 = d.dev;
  d.ctx.r4.u64 = shader;
  d.ctx.r5.u64 = declaration;
  d.ctx.r6.u64 = binding;
  sub_822127F8(d.ctx, d.base);
  return d.ctx.r3.u32 != 0;
}

// D3D_IncrementalShaderPatchAndLoad (sub_82656EE0): the vertex shader's
// binding copied where the GPU loads it from (the ring there, scratch here),
// patched for the pixel shader and for the declaration, and loaded.
void PatchAndLoad(Device& d, uint32_t match_ps, uint32_t shader, uint32_t program_cntl, uint32_t declaration,
                  uint32_t vs_info, uint32_t ps_info, uint32_t binding) {
  const uint32_t info = U32(d.base, shader + (binding + 112) * 8) + shader;
  const uint32_t bytes = U32(d.base, info + 876);
  const uint32_t dwords = bytes >> 2;
  const uint32_t copy = ScratchAlloc(bytes);
  if (!copy) return;
  const uint32_t source = Cpu(Phys(U32(d.base, info + 872) + U32(d.base, shader + 32)));
  std::memcpy(d.base + copy, d.base + source, bytes);
  if (match_ps && (U8(d.base, vs_info + 8) & 7) != 7) {
    d.ctx.r3.u64 = vs_info;
    d.ctx.r4.u64 = copy;
    d.ctx.r5.u64 = program_cntl;
    d.ctx.r6.u64 = ps_info;
    sub_82656DA0(d.ctx, d.base);
  }
  const uint8_t flags = U8(d.base, d.dev + 10942);
  S8(d.base, d.dev + 10942, uint8_t((flags & 0x7F) | (match_ps ? 0x80 : 0)));
  d.ctx.r3.u64 = shader;
  d.ctx.r4.u64 = copy;
  d.ctx.r5.u64 = declaration;
  d.ctx.r6.u64 = d.dev + 12528;
  d.ctx.r7.u64 = binding;
  sub_821E5218(d.ctx, d.base);
  S64(d.base, d.dev + 11840, U64(d.base, d.dev + 12536));
  S64(d.base, d.dev + 11832, U64(d.base, d.dev + 12528));
  d.out.Shader(0, Phys(copy), dwords, Hash(d, Phys(copy), dwords));
  g_counts.patched_loads.fetch_add(1, std::memory_order_relaxed);
}

// SetPending_HiZEnable (sub_8222A798): works out whether hierarchical Z can
// stay on (device +10560 bit 1) and asks for 0x2203 again. Its cache flush
// event has no host counterpart.
uint64_t PendingHiZEnable(Device& d, uint64_t mask) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  uint32_t ps_kills = 0;
  if (const uint32_t ps = U32(base, dev + kPixelShader)) {
    const uint32_t info = U32(base, ps + 64) + ps;
    ps_kills = (U32(base, info + 48) >> 27) & 1;
  }
  const uint32_t depth = U32(base, dev + kRegs2200);  // RB_DEPTHCONTROL
  const uint32_t r7 = depth >> 1;
  const uint32_t hiz = U32(base, dev + 10560);
  uint32_t r11 = ((uint32_t(U8(base, dev + 10942)) >> 5) & r7);
  r11 = (r11 & 1) | (hiz & ~1u);
  S32(base, dev + 10560, r11);
  const uint32_t stencil = (U32(base, dev + 11852) >> 20) & 0xFFF;
  uint32_t enable;
  if (!(stencil & 2)) {
    enable = stencil & 1;
  } else {
    const uint32_t zfunc = (depth >> 4) & 7;
    bool ok;
    if (r11 & 0x10) {
      ok = zfunc == 4 || zfunc == 6 || zfunc == 2;
    } else {
      ok = zfunc == 1 || zfunc == 3 || zfunc == 2;
    }
    enable = r11 & ~ps_kills & r7 & (ok ? 1u : 0u) & 1u;
    if (depth & 1) {
      // Stencil on: only while the stencil ops (front, and back when two
      // sided) leave the buffer alone.
      enable &= ((depth & 0x00003800u) ? 0 : 1) & ((depth & 0x000E0000u) ? 0 : 1);
      if (depth & 0x80) enable &= ((depth & 0xE0000000u) ? 0 : 1) & ((depth & 0x03800000u) ? 0 : 1);
    }
  }
  uint32_t v = U32(base, dev + 10560);
  v = (v & ~2u) | ((enable & 1) << 1);
  S32(base, dev + 10560, v);
  const uint8_t b41 = U8(base, dev + 10941);
  if (v & 2) {
    if ((b41 & 4) && (U32(base, dev + 13504) == 0 || U32(base, dev + 13512) == 1)) {
      S8(base, dev + 10941, b41 & 0xFB);
      g_counts.hiz_events.fetch_add(1, std::memory_order_relaxed);
    }
  } else if ((r7 & 1) && (depth & 4)) {
    const uint32_t zfunc = (depth >> 4) & 7;
    bool set = zfunc == 7 || zfunc == 5;
    if (!set) set = (v & 0x10) ? (zfunc == 1 || zfunc == 3) : (zfunc == 4 || zfunc == 6);
    if (set) S8(base, dev + 10941, b41 | 4);
  }
  return mask | 0x100;
}

// SetPending_Shaders (sub_8222BF10): the shaders' microcode loads, their
// literal constants, the vertex shader's binding to the declaration, and the
// render states that follow from the pair (EDRAM mode, program control).
// Predicated tiling's per-band variants are written once, unpredicated: the
// host draws the scene in one pass.
uint64_t PendingShaders(Device& d, uint64_t r20) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  int32_t r15 = -1, r19 = -1;
  if (r20 & 0x20000) r20 = PendingHiZEnable(d, r20);
  const uint32_t vs = U32(base, dev + kVertexShader);
  const uint32_t decl = U32(base, dev + kDeclaration);
  const uint32_t ps = U32(base, dev + kPixelShader);
  if (!vs) return r20;
  uint32_t r24 = 0;
  uint32_t r25 = U32(base, vs + 896) + vs + 872;
  uint32_t r28 = 0, r18 = 0, r17 = 0, r27 = 0;
  if (!ps) {
    const uint8_t b43 = U8(base, dev + 10943);
    if (b43 & 1) {
      d.out.Reg(0x05C8, 0x20000);
      S8(base, dev + 10943, b43 & 0xFE);
    }
    if (U32(base, vs + 872) & 0x20) {
      r24 = 1;
      r25 = U32(base, vs + 904) + vs + 872;
    }
    uint32_t mc = U32(base, dev + 10580);
    if ((mc & 7) != 5) {
      mc = (mc & ~7u) | 5;
      r20 |= 0x80008;
      S32(base, dev + 10580, mc);
      if (U8(base, dev + 10943) & 8) {
        r20 |= 1ull << 56;
        S32(base, dev + 10372, U32(base, dev + 10372) & ~0x000F0000u);
      }
    }
    r27 = (U32(base, r25 + 20) & 0x1F) != 0;
  } else {
    const uint32_t info = U32(base, ps + 64) + ps;
    r28 = info + 40;
    r18 = U32(base, info + 48);
    r17 = U32(base, info + 52);
    const uint32_t t = (r18 & 0x70000000u) > 0x10000000u ? 1 : 0;
    const uint8_t b42 = U8(base, dev + 10942), b43 = U8(base, dev + 10943);
    S8(base, dev + 10943, uint8_t(b43 | t));
    S8(base, dev + 10942, uint8_t(b42 | (t << 3)));
    if (r20 & 0x100000) {
      LiteralConstants(d, ps + 40, U32(base, ps + 24));
      r20 |= 0x4000;
      const uint32_t address = Phys(U32(base, info + 40) + U32(base, ps + 24));
      const uint32_t dwords = U32(base, info + 44) >> 2;
      d.out.Shader(1, address, dwords, Hash(d, address, dwords));
      g_counts.shader_loads.fetch_add(1, std::memory_order_relaxed);
      uint32_t mc = U32(base, dev + 10580);
      S32(base, dev + 10536, U32(base, r28 + 24));
      if ((mc & 7) != 4) {
        mc = (mc & ~7u) | 4;
        r20 |= 0x80008;
        S32(base, dev + 10580, mc);
        if (U8(base, dev + 10943) & 8) {
          r20 |= 1ull << 56;
          S32(base, dev + 10372, U32(base, U32(base, dev + 12440) + 28));
        }
      }
    }
    r27 = ((U32(base, r28 + 20) ^ U32(base, r25 + 20)) & 0x7FFFF) != 0;
  }
  // loc_8222C1D0
  const uint32_t slot = ProgramSlot();
  if (!slot) return r20;
  S32(base, slot, U32(base, r25 + 8));
  const uint32_t r26 = U32(base, dev + 10580);
  const uint32_t r23 = U32(base, r25 + 12);
  uint32_t r29 = r26;
  if (U8(base, dev + 10940) & 0x40) {
    bool keep = false;
    if (r28) {
      const uint32_t f = U32(base, r28 + 28);
      keep = (U32(base, dev + 10556) & 8) || (f & 0x10) || (f & 0x20);
    }
    if (!keep) r29 = (r29 & ~7u) | 5;
    if (r29 != U32(base, dev + 12700) || (r20 & 8)) {
      r20 |= 8;
      S32(base, dev + 12700, r29);
    }
  }
  // loc_8222C26C
  const uint8_t b42 = U8(base, dev + 10942);
  if ((b42 & 0x80) || r27) {
    if (r20 & 0x180000) {
      S8(base, dev + 10942, b42 & 0xBF);
      LiteralConstants(d, vs + 872, U32(base, vs + 32));
      PatchAndLoad(d, r27, vs, slot, decl, r25, r28, r24);
    }
  } else if (r20 & 0x80000) {
    const uint32_t vinfo = vs + 872;
    LiteralConstants(d, vinfo, U32(base, vs + 32));
    S8(base, dev + 10942, U8(base, dev + 10942) & 0xBF);
    bool second = false;
    if (decl && !(U32(base, vinfo) & 0x40)) {
      if (U8(base, dev + 10940) & 0x80) {
        PatchAndLoad(d, 0, vs, slot, decl, r25, r28, r24);
      } else {
        if (BindingMatches(d, vs, decl, r24)) {
          r19 = int32_t(r24);
        } else {
          PatchAndLoad(d, 0, vs, slot, decl, r25, r28, r24);
        }
        second = ((r29 ^ r26) & 7) && (U32(base, vinfo) & 0x20) && BindingMatches(d, vs, decl, 1);
      }
    } else {
      r19 = int32_t(r24);
      second = ((r29 ^ r26) & 7) && (U32(base, vinfo) & 0x20);
    }
    if (second) {
      S8(base, dev + 10942, U8(base, dev + 10942) | 0x40);
      r15 = 1;
    }
  }
  // loc_8222C3CC
  if (r20 & 0x180000) {
    S32(base, dev + 10532, r23 | r17);
    r20 |= 0x18000;
    S32(base, dev + 10528, U32(base, slot) | r18);
  }
  auto load_binding = [&](int32_t binding) {
    const uint32_t info = U32(base, vs + (uint32_t(binding) + 112) * 8) + vs;
    const uint32_t address = Phys(U32(base, info + 872) + U32(base, vs + 32));
    const uint32_t dwords = U32(base, info + 876) >> 2;
    d.out.Shader(0, address, dwords, Hash(d, address, dwords));
    S32(base, vs + uint32_t(binding) * 416 + 64, U32(base, dev + 10908));
    g_counts.shader_loads.fetch_add(1, std::memory_order_relaxed);
  };
  if (U8(base, dev + 10940) & 0x40) {
    // Predicated tiling: the band variants once, unpredicated.
    g_counts.predicated_tiling.fetch_add(1, std::memory_order_relaxed);
    if (r20 & 8) {
      d.out.Reg(0x2208, U32(base, dev + 10580));
      r20 &= ~uint64_t(8);
    }
    if (r20 & (1ull << 56)) {
      d.out.Reg(0x2001, U32(base, dev + 10372));
      r20 &= ~(1ull << 56);
    }
    if (r20 & 0x10000) {
      d.out.RegsFromGuest(base, 0x2180, dev + 10528, 2);
      r20 &= ~uint64_t(0x18000);
    }
    if (r19 != -1) {
      load_binding(r19);
    } else if (r15 != -1) {
      load_binding(r15);
    }
  } else if (r19 != -1) {
    load_binding(r19);
  }
  return r20;
}

// The flush every draw function begins with, over the given dirty masks
// (all of them for a draw; the inherited ones for a command buffer's run).
// A block's dirty word is cleared once anything of it was sent, as Direct3D
// does.
void Flush(Device& d, const uint64_t (&masks)[5]) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  g_counts.flushes.fetch_add(1, std::memory_order_relaxed);
  const uint64_t d0 = masks[0], d1 = masks[1], d3 = masks[3], d4 = masks[4];
  uint64_t d2 = masks[2];
  if (d0) {
    AluConstants(d, d0, 0x4000, dev + kVsConstants);
    S64(base, dev + 0, 0);
  }
  if (d1) {
    AluConstants(d, d1, 0x4400, dev + kPsConstants);
    S64(base, dev + 8, 0);
  }
  if (d2) {
    if (d2 & 0x1E0000) d2 = PendingShaders(d, d2);
    if (U64(base, dev + kPredicatedMask) & d2) {
      // SetPending_Predicated (sub_8226AAB8): predicated tiling's band
      // windows. One pass on the host; counted to see it never comes.
      g_counts.set_pending_predicated.fetch_add(1, std::memory_order_relaxed);
    }
    if (d2 & 0xFFF) RenderStates(d, d2 << 52, 0x2200, dev + kRegs2200);
    if (d2 & 0x1F000) RenderStates(d, (d2 << 47) & 0xF800000000000000ull, 0x2180, dev + kRegs2180);
    if (d2 & (0xFFFFull << 42)) RenderStates(d, (d2 << 6) & 0xFFFF000000000000ull, 0x2000, dev + kRegs2000);
    if (d2 & 0x000003FFFFE00000ull) {
      RenderStates(d, (d2 << 22) & 0xFFFFF80000000000ull, 0x2100, dev + kRegs2100);
    }
    S64(base, dev + 16, 0);
  }
  if (d3) {
    if (d3 & 0xFFFFFFFFull) FetchConstants(d, d3 << 32);
    if (d3 & 0x007FFFFC00000000ull) {
      RenderStates(d, (d3 << 9) & 0xFFFFF80000000000ull, 0x2280, dev + kRegs2280);
    }
    S64(base, dev + 24, 0);
  }
  if (d4) {
    if (d4 & 0x3FFFFFFFFFull) RenderStates(d, (d4 << 26) & 0xFFFFFFFFFC000000ull, 0x2300, dev + kRegs2300);
    if (d4 & (0xFFull << 38)) RenderStates(d, (d4 << 18) & 0xFF00000000000000ull, 0x2380, dev + kRegs2380);
    if (d4 & (0x3Full << 49)) ClipPlanes(d, (d4 << 9) & 0xFC00000000000000ull);
    if (d4 & (1ull << 56)) RenderStates(d, 0xFFFFFFFFFF000000ull, 0x4900, dev + kBoolLoop);
    S64(base, dev + 32, 0);
  }
}

// The render-state part of the flush alone, as D3D_ClearF and Resolve do it
// before their own packets (sub_82223560, sub_8222D6C8): only the bits sent
// are cleared.
void FlushRenderStates(Device& d) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  uint64_t d2 = U64(base, dev + 16);
  if (d2) {
    if (U64(base, dev + kPredicatedMask) & d2) g_counts.set_pending_predicated.fetch_add(1, std::memory_order_relaxed);
    if (d2 & 0xFFF) {
      RenderStates(d, d2 << 52, 0x2200, dev + kRegs2200);
      d2 &= ~0xFFFull;
    }
    if (d2 & 0x1F000) {
      RenderStates(d, (d2 << 47) & 0xF800000000000000ull, 0x2180, dev + kRegs2180);
      d2 &= ~0x1F000ull;
    }
    if (d2 & (0xFFFFull << 42)) {
      RenderStates(d, (d2 << 6) & 0xFFFF000000000000ull, 0x2000, dev + kRegs2000);
      d2 &= ~(0xFFFFull << 42);
    }
    if (d2 & 0x000003FFFFE00000ull) {
      RenderStates(d, (d2 << 22) & 0xFFFFF80000000000ull, 0x2100, dev + kRegs2100);
      d2 &= ~0x000003FFFFE00000ull;
    }
    S64(base, dev + 16, d2);
  }
  const uint64_t d3 = U64(base, dev + 24);
  if (d3 & 0x007FFFFC00000000ull) {
    RenderStates(d, (d3 << 9) & 0xFFFFF80000000000ull, 0x2280, dev + kRegs2280);
    S64(base, dev + 24, d3 & ~0x007FFFFC00000000ull);
  }
  const uint64_t d4 = U64(base, dev + 32);
  if (d4 & 0x3FFFFFFFFFull) {
    RenderStates(d, (d4 << 26) & 0xFFFFFFFFFC000000ull, 0x2300, dev + kRegs2300);
    S64(base, dev + 32, d4 & ~0x3FFFFFFFFFull);
  }
}

void FlushAll(Device& d) {
  const uint64_t masks[5] = {U64(d.base, d.dev + 0), U64(d.base, d.dev + 8), U64(d.base, d.dev + 16),
                             U64(d.base, d.dev + 24), U64(d.base, d.dev + 32)};
  Flush(d, masks);
}

// The visibility query token every draw packet carries.
uint32_t VizToken(const uint8_t* base, uint32_t dev) {
  const uint32_t query = U8(base, dev + 12187);
  return (U8(base, dev + 12122 + query) & 0x3F) | (query ? 0x100 : 0);
}

// Large draws are split the way Direct3D splits them (65535 indices a
// packet, continued strips overlapping).
uint32_t SplitCount(const uint8_t* base, uint32_t primitive, uint32_t count) {
  if (count <= 65535) return count;
  const uint32_t divisor = U32(base, kPrimitiveSplit + primitive * 8);
  if (!divisor) return count;
  return ((65535 / divisor) & ~1u) * divisor;
}

// Verify mode: the device fields the native flush may change are put back
// before the game's own Direct3D runs, so it flushes exactly as it would.
struct Snapshot {
  static constexpr std::array<std::pair<uint32_t, uint32_t>, 5> kRanges{
      {{0, 64}, {10100, 10960}, {11800, 11900}, {12500, 12760}, {13440, 13520}}};
  std::array<uint8_t, 64 + 860 + 100 + 260 + 80> bytes;
  void Save(const uint8_t* base, uint32_t dev) {
    size_t at = 0;
    for (auto [b, e] : kRanges) {
      std::memcpy(bytes.data() + at, base + dev + b, e - b);
      at += e - b;
    }
  }
  void Restore(uint8_t* base, uint32_t dev) const {
    size_t at = 0;
    for (auto [b, e] : kRanges) {
      std::memcpy(base + dev + b, bytes.data() + at, e - b);
      at += e - b;
    }
  }
};

// Runs `fill` on the device's current list, restoring the device in verify
// mode (where the game's Direct3D still runs after) and the PPC registers
// the guest calls inside used.
template <typename Fill>
void Capture(PPCContext& ctx, uint8_t* base, uint32_t dev, Fill&& fill, List* target = nullptr) {
  const bool verify = GetMode() == Mode::kVerify;
  Snapshot snapshot;
  if (verify) snapshot.Save(base, dev);
  PPCContext saved = ctx;
  Device d{base, dev, ctx, Builder(target ? *target : Current(dev)), verify};
  fill(d);
  ctx = saved;
  if (verify) snapshot.Restore(base, dev);
}

// Verify mode: a type-3 NOP the command processor recognises, around a
// Direct3D call whose PM4 draws the native lists do not hold. Written the
// way Direct3D writes a packet (more ring first if past the end).
void EmitNopMarker(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t payload) {
  uint32_t p = U32(base, dev + kRingWrite);
  if (p > U32(base, dev + kRingEnd)) {
    PPCContext saved = ctx;
    ctx.r3.u64 = dev;
    sub_82210AB0(ctx, base);
    p = ctx.r3.u32;
    ctx = saved;
  }
  S32(base, p + 4, 0xC0011000u);  // type 3, NOP, two dwords
  S32(base, p + 8, kNopMagic);
  S32(base, p + 12, payload);
  S32(base, dev + kRingWrite, p + 12);
}

// ---------------------------------------------------------------------------
// Handlers.

// D3DDevice_DrawIndexedVertices(device, primitive, base vertex, start index, count)
bool DrawIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t first = ctx.r6.u32, total = ctx.r7.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  std::unique_lock lock(g_front.mutex);
  Capture(ctx, base, dev, [&](Device& d) {
    FlushAll(d);
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    const uint32_t ib_flags = U32(base, ib + 0), ib_base = U32(base, ib + 24);
    const bool wide = (ib_flags & 0x80000000u) != 0;
    const uint32_t endian = (ib_flags << 1) & 0xC0000000u;
    const uint32_t viz = VizToken(base, dev);
    const uint32_t extent = (U8(base, dev + 10940) & 1) ? kDrawScreenExtent : 0;
    uint32_t start = first, count = total;
    while (true) {
      const uint32_t chunk = SplitCount(base, primitive, count);
      d.out.Reg(0x2102, base_vertex);
      uint32_t initiator = (chunk << 16) | (primitive & 0x3F);
      uint32_t address, size;
      if (wide) {
        initiator |= 0x800;
        address = Phys(start * 4 + ib_base);
        size = ((chunk * 2) & 0x00FFFFFE) | endian;
      } else {
        address = Phys(start * 2 + ib_base);
        size = (chunk & 0x00FFFFFF) | endian;
      }
      if (extent) g_counts.screen_extent.fetch_add(1, std::memory_order_relaxed);
      d.out.Draw(kDrawPredicated | extent, viz, initiator, address, size, serial);
      g_counts.draws.fetch_add(1, std::memory_order_relaxed);
      const uint32_t left = count - chunk;
      if (!left) break;
      const uint32_t overlap = U32(base, kPrimitiveSplit + primitive * 8 + 4);
      start += chunk - overlap;
      count = left + overlap;
    }
  });
  lock.unlock();
  if (GetMode() == Mode::kNative) return true;
  EmitNopMarker(ctx, base, dev, kTagPayload | (serial & 0xFFFFFF));
  return false;
}

// D3DDevice_DrawVertices(device, primitive, start vertex, count)
bool DrawVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, first = ctx.r5.u32, total = ctx.r6.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  std::unique_lock lock(g_front.mutex);
  Capture(ctx, base, dev, [&](Device& d) {
    FlushAll(d);
    const uint32_t viz = VizToken(base, dev);
    const uint32_t extent = (U8(base, dev + 10940) & 1) ? kDrawScreenExtent : 0;
    uint32_t start = first, count = total;
    while (true) {
      const uint32_t chunk = SplitCount(base, primitive, count);
      d.out.Reg(0x2102, start);
      if (extent) g_counts.screen_extent.fetch_add(1, std::memory_order_relaxed);
      d.out.Draw(kDrawPredicated | extent, viz, (chunk << 16) | (primitive & 0x3F) | 0x80, 0, 0, serial);
      g_counts.draws.fetch_add(1, std::memory_order_relaxed);
      const uint32_t left = count - chunk;
      if (!left) break;
      const uint32_t overlap = U32(base, kPrimitiveSplit + primitive * 8 + 4);
      start += chunk - overlap;
      count = left + overlap;
    }
  });
  lock.unlock();
  if (GetMode() == Mode::kNative) return true;
  EmitNopMarker(ctx, base, dev, kTagPayload | (serial & 0xFFFFFF));
  return false;
}

// Verify mode: the BeginVertices record waiting for the data address the
// game's Direct3D picks.
// Its records wait in a list of their own until the game's BeginVertices
// returned, then join the current list: a kick-off inside it may publish
// the current list, which the command processor then owns.
struct PendingBegin {
  List records;
  size_t fetch_at = 0;
  bool waiting = false;
} g_pending_begin;

// D3DDevice_BeginVertices(device, primitive, vertex count, stride) -> data.
// The vertices go to vertex fetch constant 95 (fetch slot 31), the slot's
// dirty bit is set again so the next draw puts the game's value back, and
// the inlined EndVertices sets the write pointer to +13444.
bool BeginVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  std::unique_lock lock(g_front.mutex);
  g_counts.begin_vertices.fetch_add(1, std::memory_order_relaxed);
  const bool native = GetMode() == Mode::kNative;
  const uint32_t dwords = (count * stride) >> 2;
  uint32_t data = 0;
  if (native) {
    data = ScratchAlloc(dwords * 4);
    if (!data) {
      ctx.r3.u64 = 0;
      return true;
    }
  }
  Capture(ctx, base, dev, [&](Device& d) {
    const uint32_t stride_dwords = (stride >> 2) & 0xFF;
    const uint8_t old_stride = U8(base, dev + 12528);
    S8(base, dev + 12528, uint8_t(stride_dwords));
    if (stride_dwords != U8(base, dev + 11832) && U32(base, dev + kVertexShader)) {
      S64(base, dev + 16, U64(base, dev + 16) | 0x80000);
    }
    S64(base, dev + 24, U64(base, dev + 24) & ~1ull);
    FlushAll(d);
    S8(base, dev + 12528, old_stride);
    d.out.Reg(0x2102, 0);
    const uint32_t fetch[6] = {1, 0, 1, 0, Phys(data) | 3, 0x10000002u | ((dwords << 2) & 0x03FFFFFCu)};
    const size_t fetch_at = d.out.list().words.size();
    d.out.Regs(0x4800 + 31 * 6, fetch, 6);
    d.out.Draw(kDrawPredicated, VizToken(base, dev), (count << 16) | (primitive & 0x3F) | 0x80, 0, 0, serial);
    g_counts.draws.fetch_add(1, std::memory_order_relaxed);
    if (!native) {
      g_pending_begin.fetch_at = fetch_at;
      g_pending_begin.waiting = true;
    }
  }, native ? nullptr : &g_pending_begin.records);
  if (!native) {
    lock.unlock();
    EmitNopMarker(ctx, base, dev, kTagPayload | (serial & 0xFFFFFF));
    return false;
  }
  S64(base, dev + 24, U64(base, dev + 24) | 1);
  S32(base, dev + 13444, U32(base, dev + kRingWrite));
  S32(base, dev + 13448, data);
  S32(base, dev + 13456, dwords);
  ctx.r3.u64 = data;
  return true;
}

// Verify mode: patch in the data address the game's BeginVertices chose.
void AfterBeginVertices(PPCContext& ctx, uint32_t dev) {
  std::lock_guard lock(g_front.mutex);
  if (!g_pending_begin.waiting) return;
  List& records = g_pending_begin.records;
  if (const uint32_t data = ctx.r3.u32) {
    // [op, first, v0..v5]: v4 holds the address.
    records.words[g_pending_begin.fetch_at + 2 + 4] = Phys(data) | 3;
    List& list = Current(dev);
    list.words.insert(list.words.end(), records.words.begin(), records.words.end());
    list.draws += records.draws;
  }
  records.words.clear();
  records.draws = 0;
  g_pending_begin.waiting = false;
}

// Calls whose PM4 draws the native lists do not hold yet. The state left
// pending before them is flushed into the list when they flush it
// themselves; in verify mode their PM4 is fenced by NOP markers.
thread_local uint32_t t_opaque_depth = 0;
thread_local uint32_t t_opaque_serial = 0;

enum OpaqueFlush { kFlushNone, kFlushAll, kFlushRenderStates };
void OpaqueBegin(PPCContext& ctx, uint8_t* base, uint32_t kind, OpaqueFlush flush) {
  const uint32_t dev = ctx.r3.u32;
  if (t_opaque_depth++) return;
  uint32_t serial;
  {
    std::lock_guard lock(g_front.mutex);
    g_counts.markers.fetch_add(1, std::memory_order_relaxed);
    serial = ++g_front.marker_serial & 0xFFFF;
    if (flush == kFlushAll) Capture(ctx, base, dev, [&](Device& d) { FlushAll(d); });
    if (flush == kFlushRenderStates) Capture(ctx, base, dev, [&](Device& d) { FlushRenderStates(d); });
    Builder(Current(dev)).Marker(kind, serial);
  }
  t_opaque_serial = serial;
  EmitNopMarker(ctx, base, dev, (kind << 24) | 0x10000 | serial);
}

void OpaqueEnd(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t kind) {
  if (!t_opaque_depth || --t_opaque_depth) return;
  EmitNopMarker(ctx, base, dev, (kind << 24) | t_opaque_serial);
}

// D3DDevice_BeginCommandBuffer(device, command buffer, flags, inherit, ...):
// a list of its own from here to EndCommandBuffer.
void BeginCommandBuffer(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  g_counts.recordings.fetch_add(1, std::memory_order_relaxed);
  auto [it, added] = g_front.recording.try_emplace(ctx.r3.u32);
  if (added) g_counts.devices.fetch_add(1, std::memory_order_relaxed);
  if (it->second) g_counts.nested_recordings.fetch_add(1, std::memory_order_relaxed);
  it->second = std::make_shared<List>();
  it->second->command_buffer = ctx.r4.u32;
}

// D3DDevice_EndCommandBuffer(device): Direct3D flushes what is still pending
// into the buffer (when +108 bit 15 is set) before closing it.
void EndCommandBuffer(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  std::lock_guard lock(g_front.mutex);
  auto it = g_front.recording.find(dev);
  if (it == g_front.recording.end() || !it->second) return;
  const uint32_t cb = U32(base, dev + kRecording);
  if (cb && (U32(base, cb + 108) & 0x8000)) Capture(ctx, base, dev, [&](Device& d) { FlushAll(d); });
  g_front.command_buffers[it->second->command_buffer] = it->second;
  it->second.reset();
}

// D3DDevice_RunCommandBuffer(device, command buffer, ...): the pending state
// the buffer inherits goes first (dirty & the buffer's inherit masks at
// +24..+56), then the buffer. Direct3D then marks dirty what the buffer sets.
void RunCommandBuffer(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, cb = ctx.r4.u32;
  std::lock_guard lock(g_front.mutex);
  g_counts.runs.fetch_add(1, std::memory_order_relaxed);
  Capture(ctx, base, dev, [&](Device& d) {
    uint64_t masks[5];
    for (uint32_t i = 0; i < 5; ++i) masks[i] = U64(base, dev + i * 8) & U64(base, cb + 24 + i * 8);
    Flush(d, masks);
    auto it = g_front.command_buffers.find(cb);
    if (it == g_front.command_buffers.end()) {
      g_counts.unknown_runs.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    d.out.Run(it->second);
  });
}

// D3DDevice_SetPredication(device, mask): SET_BIN_MASK_LO, 0 meaning all.
void SetPredication(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  Builder(Current(ctx.r3.u32)).Bin(0, ctx.r4.u32 ? ctx.r4.u32 : 0xFFFFFFFFu);
}

// D3DDevice_SetCommandBufferPredication(device, lo, hi): SET_BIN_MASK_LO/HI,
// both 0 meaning all.
void SetCommandBufferPredication(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  const uint32_t lo = ctx.r4.u32, hi = ctx.r5.u32;
  Builder b(Current(ctx.r3.u32));
  if (!lo && !hi) {
    b.Bin(0, 0xFFFFFFFFu);
    b.Bin(1, 0);
  } else {
    b.Bin(0, lo);
    b.Bin(1, hi);
  }
}

// SetSurfaceClip (sub_82223270, under SetScissorRect and others): the
// window scissor, written straight into the ring (0x2080-0x2082), never
// kept in the device.
void SetSurfaceClip(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  std::lock_guard lock(g_front.mutex);
  g_counts.surface_clips.fetch_add(1, std::memory_order_relaxed);
  if (U8(base, dev + 10940) & 0x30) g_counts.tiled_surface_clips.fetch_add(1, std::memory_order_relaxed);
  const uint32_t x1 = ctx.r4.u32, y1 = ctx.r5.u32, x2 = ctx.r6.u32, y2 = ctx.r7.u32;
  const uint32_t values[3] = {0, ((y1 << 16) & 0x3FFF0000u) | (x1 & 0x3FFF), ((y2 << 16) & 0x3FFF0000u) | (x2 & 0x3FFF)};
  Builder(Current(dev)).Regs(0x2080, values, 3);
}

void Swap(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  g_front.main_device = ctx.r3.u32;
  Builder(*g_front.immediate).Swap(ctx.r4.u32, 0, 0);
  Publish();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points.

Mode GetMode() {
  int32_t mode = g_mode.load(std::memory_order_relaxed);
  if (mode < 0) {
    mode = std::clamp(REXCVAR_GET(renut_native_d3d), 0, 2);
    g_mode.store(mode, std::memory_order_relaxed);
    if (mode) REXLOG_INFO("Renut: native Direct3D front end, mode {}", mode);
  }
  return Mode(mode);
}

bool OnEnter(uint32_t function, PPCContext& ctx, uint8_t* base) {
  using namespace renut::d3d;
  if (function < g_calls.size()) g_calls[function].fetch_add(1, std::memory_order_relaxed);
  switch (Function(function)) {
    case kD3DDevice_DrawIndexedVertices:
      if (t_opaque_depth) return false;
      return DrawIndexedVertices(ctx, base);
    case kD3DDevice_DrawVertices:
      if (t_opaque_depth) return false;
      return DrawVertices(ctx, base);
    case kD3DDevice_BeginVertices:
      if (t_opaque_depth) return false;
      return BeginVertices(ctx, base);
    case kD3DDevice_BeginIndexedVertices:
      OpaqueBegin(ctx, base, kMarkIndexedUP, kFlushAll);
      return false;
    case kD3DDevice_DrawIndexedTessellatedVertices:
      OpaqueBegin(ctx, base, kMarkTessellated, kFlushAll);
      return false;
    case kD3DDevice_Clear:
    case kD3DDevice_ClearF:
    case kD3D_ClearF:
      OpaqueBegin(ctx, base, kMarkClear, kFlushRenderStates);
      return false;
    case kD3DDevice_Resolve:
      OpaqueBegin(ctx, base, kMarkResolve, kFlushRenderStates);
      return false;
    case kD3DDevice_BeginExport:
    case kD3DDevice_EndExport:
      OpaqueBegin(ctx, base, kMarkExport, kFlushNone);
      return false;
    case kD3DDevice_SetShaderGPRAllocation:
      OpaqueBegin(ctx, base, kMarkGprAllocation, kFlushNone);
      return false;
    case kD3DDevice_BeginCommandBuffer:
      BeginCommandBuffer(ctx);
      return false;
    case kD3DDevice_EndCommandBuffer:
      EndCommandBuffer(ctx, base);
      return false;
    case kD3DDevice_RunCommandBuffer:
      RunCommandBuffer(ctx, base);
      return false;
    case kD3DDevice_SetPredication:
      SetPredication(ctx);
      return false;
    case kD3DDevice_SetCommandBufferPredication:
      SetCommandBufferPredication(ctx);
      return false;
    case kD3D_SetSurfaceClip:
      if (t_opaque_depth) return false;
      SetSurfaceClip(ctx, base);
      return false;
    case kD3DDevice_Swap:
      Swap(ctx);
      return false;
    case kD3D_CDevice_KickOff:
    case kD3D_CDevice_KickOffSegment: {
      std::lock_guard lock(g_front.mutex);
      Publish();
      return false;
    }
    default:
      return false;
  }
}

void OnLeave(uint32_t function, uint32_t r3, PPCContext& ctx, uint8_t* base) {
  using namespace renut::d3d;
  switch (Function(function)) {
    case kD3DDevice_BeginVertices:
      if (!t_opaque_depth) AfterBeginVertices(ctx, r3);
      break;
    case kD3DDevice_BeginIndexedVertices:
      OpaqueEnd(ctx, base, r3, kMarkIndexedUP);
      break;
    case kD3DDevice_DrawIndexedTessellatedVertices:
      OpaqueEnd(ctx, base, r3, kMarkTessellated);
      break;
    case kD3DDevice_Clear:
    case kD3DDevice_ClearF:
    case kD3D_ClearF:
      OpaqueEnd(ctx, base, r3, kMarkClear);
      break;
    case kD3DDevice_Resolve:
      OpaqueEnd(ctx, base, r3, kMarkResolve);
      break;
    case kD3DDevice_BeginExport:
    case kD3DDevice_EndExport:
      OpaqueEnd(ctx, base, r3, kMarkExport);
      break;
    case kD3DDevice_SetShaderGPRAllocation:
      OpaqueEnd(ctx, base, r3, kMarkGprAllocation);
      break;
    default:
      break;
  }
}

void TakePublished(std::vector<std::shared_ptr<const List>>& out) {
  std::lock_guard lock(g_front.published_mutex);
  while (!g_front.published.empty()) {
    out.push_back(std::move(g_front.published.front()));
    g_front.published.pop_front();
  }
}

// ---------------------------------------------------------------------------
// Verifier: walks the published lists as the command processor walks the
// PM4, keeping the registers the lists set, and at every PM4 draw compares
// the draw the list holds with the one the PM4 holds.

namespace {

struct Verifier {
  std::vector<uint32_t> regs = std::vector<uint32_t>(0x5003, 0);
  std::bitset<0x5003> known;
  uint64_t vs_hash = 0, ps_hash = 0;
  uint32_t vs_dwords = 0, ps_dwords = 0;
  bool vs_known = false, ps_known = false;
  uint64_t bin_mask = 0xFFFFFFFFull, bin_select = 0xFFFFFFFFull;
  struct Frame {
    std::shared_ptr<const List> list;
    size_t pos = 0;
  };
  std::vector<Frame> stack;
  std::deque<std::shared_ptr<const List>> queue;
  uint32_t opaque = 0;
  uint32_t tag = 0;
  bool tag_valid = false;
  uint64_t untagged = 0, behind = 0, markers_passed = 0;
  uint32_t opaque_kind = 0;

  // Window statistics.
  uint64_t first_frame = 0, frames = 0, pm4_draws = 0, compared = 0, clean = 0, dirty = 0;
  uint64_t skipped = 0, skipped_agree = 0, opaque_draws = 0, missing = 0, native_extra = 0;
  uint64_t unexpected_markers = 0, marker_searches = 0, marker_lost = 0, bad_records = 0;
  uint64_t f_initiator = 0, f_dma = 0, f_vs = 0, f_ps = 0, f_bin = 0, f_vs_unknown = 0;
  std::array<uint64_t, 8> opaque_by_kind{};
  std::unordered_map<uint32_t, uint64_t> reg_mismatch;
  std::string details;
  uint32_t detail_lines = 0;
  std::string unmatched;
  uint32_t unmatched_lines = 0;
  uint64_t reports = 0;
  std::string path;
} g_verify;

std::mutex g_verify_path_mutex;

const uint32_t* Peek(Verifier& v) {
  for (int guard = 0; guard < 1000000; ++guard) {
    if (v.stack.empty()) {
      if (v.queue.empty()) {
        std::vector<std::shared_ptr<const List>> more;
        TakePublished(more);
        for (auto& l : more) v.queue.push_back(std::move(l));
        if (v.queue.empty()) return nullptr;
      }
      v.stack.push_back({std::move(v.queue.front()), 0});
      v.queue.pop_front();
    }
    Verifier::Frame& f = v.stack.back();
    const std::vector<uint32_t>& words = f.list->words;
    if (f.pos >= words.size()) {
      v.stack.pop_back();
      continue;
    }
    const uint32_t* w = words.data() + f.pos;
    const uint32_t op = w[0] & 0xFF;
    switch (op) {
      case kOpRegs: {
        const uint32_t count = w[0] >> 8, first = w[1];
        if (f.pos + 2 + count > words.size() || first + count > 0x5003) {
          ++v.bad_records;
          f.pos = words.size();
          break;
        }
        for (uint32_t i = 0; i < count; ++i) {
          v.regs[first + i] = w[2 + i];
          v.known.set(first + i);
        }
        f.pos += 2 + count;
        break;
      }
      case kOpShader: {
        const uint32_t type = w[1] & 3;
        const uint64_t hash = uint64_t(w[3]) | (uint64_t(w[4]) << 32);
        if (type == 0) {
          v.vs_hash = hash;
          v.vs_dwords = w[2];
          v.vs_known = true;
        } else {
          v.ps_hash = hash;
          v.ps_dwords = w[2];
          v.ps_known = true;
        }
        f.pos += 5;
        break;
      }
      case kOpBin: {
        const uint32_t which = w[0] >> 8, value = w[1];
        if (which == 0) v.bin_mask = (v.bin_mask & 0xFFFFFFFF00000000ull) | value;
        if (which == 1) v.bin_mask = (v.bin_mask & 0xFFFFFFFFull) | (uint64_t(value) << 32);
        if (which == 2) v.bin_select = (v.bin_select & 0xFFFFFFFF00000000ull) | value;
        if (which == 3) v.bin_select = (v.bin_select & 0xFFFFFFFFull) | (uint64_t(value) << 32);
        f.pos += 2;
        break;
      }
      case kOpRun: {
        std::shared_ptr<const List> child = w[1] < f.list->runs.size() ? f.list->runs[w[1]] : nullptr;
        f.pos += 2;
        if (child) v.stack.push_back({std::move(child), 0});
        break;
      }
      case kOpDraw:
      case kOpMarker:
      case kOpSwap:
        return w;
      default:
        ++v.bad_records;
        f.pos = words.size();
        break;
    }
  }
  return nullptr;
}

void Consume(Verifier& v) {
  if (v.stack.empty()) return;
  Verifier::Frame& f = v.stack.back();
  const uint32_t op = f.list->words[f.pos] & 0xFF;
  f.pos += op == kOpDraw ? 6 : op == kOpMarker ? 2 : op == kOpSwap ? 4 : 1;
}

// A PM4 draw may reach the command processor before the list holding it was
// published only if the kick-off publishing missed a kick; wait a moment.
// Never waited for: the command processor stalling here stalls the game's
// fences (the first verify run sat at 0 FPS).
const uint32_t* PeekWaiting(Verifier& v) { return Peek(v); }

void Detail(Verifier& v, const std::string& line) {
  if (v.detail_lines >= 60) return;
  ++v.detail_lines;
  v.details += line;
  v.details += '\n';
}

// The registers the lists are responsible for.
constexpr std::array<std::pair<uint32_t, uint32_t>, 4> kCompared{{
    {0x2000, 0x2400},   // render states (the D3D shadow blocks, the window scissor, ...)
    {0x4000, 0x4800},   // ALU constants
    {0x4800, 0x48C0},   // fetch constants
    {0x4900, 0x4928},   // booleans, loops
}};
// Registers the command processor writes for a draw itself.
// And the copy registers (RB_COPY_*, 0x2318-0x2327), which the game's
// Resolve sets for its own copy draw (the lists hold no resolves yet), and
// the cache coherency bases (COHER_DEST_BASE_*), which the host has no use for.
bool CpOwned(uint32_t reg) {
  return reg == 0x21F9 || reg == 0x21FA || reg == 0x21FB || reg == 0x21FC || reg == 0x21FD ||
         (reg >= 0x2318 && reg <= 0x2327) || (reg >= 0x2006 && reg <= 0x200D);
}

void WriteReport(Verifier& v, uint64_t frame) {
  std::string out = fmt::format("native-d3d verify report {} - frames {}..{} ({} frames)\n\n", v.reports++,
                                v.first_frame, frame, v.frames);
  const double f = double(v.frames ? v.frames : 1);
  out += fmt::format(
      "== PM4 draws/frame {:.1f}: compared {:.1f}, clean {:.1f}, with mismatches {:.1f}; predicated skips {:.1f} "
      "(native agreed {:.1f}); inside opaque calls {:.1f}\n",
      v.pm4_draws / f, v.compared / f, v.clean / f, v.dirty / f, v.skipped / f, v.skipped_agree / f,
      v.opaque_draws / f);
  out += fmt::format(
      "== alignment: untagged PM4 draws {}, tagged without native draw {}, behind {}, native extra {}, markers "
      "passed {}, unexpected markers {}, marker searches {}, markers "
      "lost {}, bad records {}\n",
      v.untagged, v.missing, v.behind, v.native_extra, v.markers_passed, v.unexpected_markers, v.marker_searches,
      v.marker_lost, v.bad_records);
  out += fmt::format("== field mismatches: initiator {}, index dma {}, vertex shader {} (unknown {}), pixel shader {}, "
                     "bin mask {}\n",
                     v.f_initiator, v.f_dma, v.f_vs, v.f_vs_unknown, v.f_ps, v.f_bin);
  out += fmt::format("== opaque calls by kind: clear {}, resolve {}, tessellated {}, export {}, indexed UP {}, "
                     "GPR allocation {}\n",
                     v.opaque_by_kind[kMarkClear], v.opaque_by_kind[kMarkResolve],
                     v.opaque_by_kind[kMarkTessellated], v.opaque_by_kind[kMarkExport],
                     v.opaque_by_kind[kMarkIndexedUP], v.opaque_by_kind[kMarkGprAllocation]);
  auto c = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  out += fmt::format(
      "== front end (total): draws {}, flushes {}, shader loads {}, patched loads {}, literal blocks {}, "
      "begin vertices {}, markers {}, runs {} (unknown {}), recordings {} (nested {}), recording devices {}, immediate "
      "calls on another device {}, publishes {}, surface clips {} (tiled {}), screen extent {}, predicated tiling {}, SetPending_Predicated {}, HiZ events {}, no scratch "
      "{}\n",
      c(g_counts.draws), c(g_counts.flushes), c(g_counts.shader_loads), c(g_counts.patched_loads),
      c(g_counts.literal_blocks), c(g_counts.begin_vertices), c(g_counts.markers), c(g_counts.runs),
      c(g_counts.unknown_runs), c(g_counts.recordings), c(g_counts.nested_recordings), c(g_counts.devices),
      c(g_counts.foreign_immediate), c(g_counts.publishes),
      c(g_counts.surface_clips), c(g_counts.tiled_surface_clips), c(g_counts.screen_extent),
      c(g_counts.predicated_tiling), c(g_counts.set_pending_predicated), c(g_counts.hiz_events),
      c(g_counts.no_scratch));
  std::vector<std::pair<uint64_t, uint32_t>> sorted;
  for (auto& [reg, n] : v.reg_mismatch) sorted.emplace_back(n, reg);
  std::sort(sorted.rbegin(), sorted.rend());
  out += fmt::format("\n== registers that differed ({} distinct)\n", sorted.size());
  for (size_t i = 0; i < std::min<size_t>(sorted.size(), 60); ++i) {
    out += fmt::format("   {:04X} {}\n", sorted[i].second, sorted[i].first);
  }
  out += "\n== first mismatches\n";
  out += v.details;
  out += "\n== first PM4 draws without a native draw\n";
  out += v.unmatched;
  {
    std::vector<std::pair<uint64_t, uint32_t>> calls;
    for (uint32_t i = 0; i < g_calls.size(); ++i) {
      if (const uint64_t n = g_calls[i].exchange(0, std::memory_order_relaxed)) calls.emplace_back(n, i);
    }
    std::sort(calls.rbegin(), calls.rend());
    out += "\n== Direct3D calls per frame\n";
    for (size_t i = 0; i < std::min<size_t>(calls.size(), 60); ++i) {
      out += fmt::format("   {:10.1f} {}\n", calls[i].first / f, kFunctionNames[calls[i].second]);
    }
  }
  std::string path;
  {
    std::lock_guard lock(g_verify_path_mutex);
    path = v.path;
  }
  if (!path.empty()) {
    if (FILE* file = std::fopen(path.c_str(), "w")) {
      std::fwrite(out.data(), 1, out.size(), file);
      std::fclose(file);
    }
  }
  // Next window.
  v.first_frame = frame;
  v.frames = v.pm4_draws = v.compared = v.clean = v.dirty = 0;
  v.skipped = v.skipped_agree = v.opaque_draws = 0;
  v.reg_mismatch.clear();
  v.details.clear();
  v.detail_lines = 0;
  v.unmatched.clear();
  v.unmatched_lines = 0;
  v.opaque_by_kind.fill(0);
  v.missing = v.native_extra = v.unexpected_markers = v.marker_searches = v.marker_lost = v.bad_records = 0;
  v.untagged = v.behind = v.markers_passed = 0;
  v.f_initiator = v.f_dma = v.f_vs = v.f_ps = v.f_bin = v.f_vs_unknown = 0;
}

}  // namespace

namespace {

// The native draw a PM4 draw's tag names, with the lists' state applied up
// to it. Native draws passed over are the native side's extra; a native draw
// newer than the tag means the PM4 draw's own record is gone.
const uint32_t* AdvanceTo(Verifier& v, uint32_t tag) {
  for (int i = 0; i < 1000000; ++i) {
    const uint32_t* w = Peek(v);
    if (!w) return nullptr;
    const uint32_t op = w[0] & 0xFF;
    if (op == kOpSwap) return nullptr;
    if (op == kOpMarker) {
      ++v.markers_passed;
      Consume(v);
      continue;
    }
    const uint32_t ahead = (tag - w[5]) & 0xFFFFFF;
    if (ahead == 0) return w;
    if (ahead < 0x800000) {
      ++v.native_extra;
      Consume(v);
      continue;
    }
    ++v.behind;
    return nullptr;
  }
  return nullptr;
}

// After a native draw is used up: its tag still names the next PM4 draw only
// when that is the next part of the same split draw.
void AfterMatch(Verifier& v) {
  Consume(v);
  const uint32_t* w = Peek(v);
  v.tag_valid = w && (w[0] & 0xFF) == kOpDraw && ((w[5] ^ v.tag) & 0xFFFFFF) == 0;
}

void Unmatched(Verifier& v, const Pm4Draw& pm4, const char* why) {
  if (v.unmatched_lines >= 40) return;
  ++v.unmatched_lines;
  v.unmatched += fmt::format("frame {} vs {:016X} ps {:016X} prim {} n {} mode {} {}\n", pm4.frame, pm4.vs_hash,
                             pm4.ps_hash, pm4.initiator & 0x3F, pm4.initiator >> 16, pm4.edram_mode, why);
}

}  // namespace

void VerifyTag(uint32_t serial) {
  g_verify.tag = serial & 0xFFFFFF;
  g_verify.tag_valid = true;
}

void VerifyDraw(const Pm4Draw& pm4) {
  Verifier& v = g_verify;
  ++v.pm4_draws;
  if (v.opaque) {
    ++v.opaque_draws;
    return;
  }
  if (!v.tag_valid) {
    ++v.untagged;  // drawn by something the front end does not replace
    Unmatched(v, pm4, "untagged");
    return;
  }
  const uint32_t* w = AdvanceTo(v, v.tag);
  if (!w) {
    ++v.missing;
    v.tag_valid = false;
    Unmatched(v, pm4, "tagged, no native draw");
    return;
  }
  ++v.compared;
  bool bad = false;
  std::string why;
  if (w[2] != pm4.initiator) {
    ++v.f_initiator;
    bad = true;
    why += fmt::format(" initiator {:08X}/{:08X}", pm4.initiator, w[2]);
  }
  if (pm4.indexed && (w[3] != pm4.dma_base || w[4] != pm4.dma_size)) {
    ++v.f_dma;
    bad = true;
    why += fmt::format(" dma {:08X}:{:08X}/{:08X}:{:08X}", pm4.dma_base, pm4.dma_size, w[3], w[4]);
  }
  if (!v.vs_known) {
    ++v.f_vs_unknown;
  } else if (v.vs_hash != pm4.vs_hash || v.vs_dwords != pm4.vs_dwords) {
    ++v.f_vs;
    bad = true;
    why += fmt::format(" vs {:016X}({})/{:016X}({})", pm4.vs_hash, pm4.vs_dwords, v.vs_hash, v.vs_dwords);
    v.vs_hash = pm4.vs_hash;
    v.vs_dwords = pm4.vs_dwords;
  }
  // Plume runs a pixel shader only in colour passes (EDRAM mode 4).
  if (pm4.edram_mode == 4 && pm4.ps_hash && v.ps_known &&
      (v.ps_hash != pm4.ps_hash || v.ps_dwords != pm4.ps_dwords)) {
    ++v.f_ps;
    bad = true;
    why += fmt::format(" ps {:016X}/{:016X}", pm4.ps_hash, v.ps_hash);
    v.ps_hash = pm4.ps_hash;
    v.ps_dwords = pm4.ps_dwords;
  }
  if ((v.bin_mask & 0xFFFFFFFFull) != (pm4.bin_mask & 0xFFFFFFFFull)) {
    ++v.f_bin;
    bad = true;
    why += fmt::format(" bin {:X}/{:X}", pm4.bin_mask, v.bin_mask);
    v.bin_mask = pm4.bin_mask;
  }
  uint32_t reg_bad = 0;
  for (auto [b, e] : kCompared) {
    if (std::memcmp(v.regs.data() + b, pm4.regs + b, (e - b) * 4) == 0) continue;
    for (uint32_t r = b; r < e; ++r) {
      if (v.regs[r] == pm4.regs[r] || CpOwned(r)) continue;
      if (!v.known.test(r)) {
        v.regs[r] = pm4.regs[r];  // never set by a list: take the PM4's
        continue;
      }
      ++v.reg_mismatch[r];
      if (reg_bad++ < 4) why += fmt::format(" r{:04X} {:08X}/{:08X}", r, pm4.regs[r], v.regs[r]);
      v.regs[r] = pm4.regs[r];
    }
  }
  if (reg_bad) bad = true;
  if (bad) {
    ++v.dirty;
    Detail(v, fmt::format("frame {} vs {:016X} ps {:016X} prim {}:{}", pm4.frame, pm4.vs_hash, pm4.ps_hash,
                          pm4.initiator & 0x3F, pm4.initiator >> 16) +
                  why + (reg_bad > 4 ? fmt::format(" (+{} registers)", reg_bad - 4) : std::string()));
  } else {
    ++v.clean;
  }
  AfterMatch(v);
}

void VerifySkipped(uint64_t bin_mask, uint64_t bin_select) {
  Verifier& v = g_verify;
  ++v.skipped;
  if (v.opaque || !v.tag_valid) return;
  const uint32_t* w = AdvanceTo(v, v.tag);
  if (!w) {
    ++v.missing;
    v.tag_valid = false;
    return;
  }
  if ((v.bin_mask & v.bin_select) == 0) ++v.skipped_agree;
  v.bin_mask = bin_mask;
  v.bin_select = bin_select;
  AfterMatch(v);
}

void VerifyMarker(uint32_t payload) {
  Verifier& v = g_verify;
  const uint32_t kind = payload >> 24, serial = payload & 0xFFFF;
  if (!(payload & 0x10000)) {
    if (v.opaque) --v.opaque;
    return;
  }
  ++v.opaque;
  if (kind < v.opaque_by_kind.size()) ++v.opaque_by_kind[kind];
  // The matching marker record, applying state on the way. Draw records
  // passed over are the native side's extra.
  for (int i = 0; i < 100000; ++i) {
    const uint32_t* w = PeekWaiting(v);
    if (!w) {
      ++v.marker_lost;
      return;
    }
    const uint32_t op = w[0] & 0xFF;
    if (op == kOpMarker && w[1] == serial) {
      Consume(v);
      return;
    }
    if (op == kOpSwap) {
      ++v.marker_lost;
      return;
    }
    if (op == kOpDraw) ++v.native_extra;
    if (op == kOpMarker) ++v.marker_searches;
    Consume(v);
  }
}

void VerifySwap(uint64_t frame) {
  Verifier& v = g_verify;
  ++v.frames;
  v.opaque = 0;
  for (int i = 0; i < 100000; ++i) {
    const uint32_t* w = PeekWaiting(v);
    if (!w) {
      ++v.missing;
      break;
    }
    const uint32_t op = w[0] & 0xFF;
    Consume(v);
    if (op == kOpSwap) break;
    if (op == kOpDraw) ++v.native_extra;
  }
  if (v.frames >= 300) WriteReport(v, frame);
}

void NativeReport(uint64_t frame, uint64_t draws, uint64_t ring_draws_dropped, uint64_t markers, uint64_t bad) {
  static uint64_t last_frame = 0, last_draws = 0, last_dropped = 0, last_markers = 0;
  if (frame < last_frame + 300) return;
  const double f = double(frame - last_frame);
  auto c = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  std::string out = fmt::format(
      "native-d3d native mode - frames {}..{}\n\n== per frame: native draws issued {:.1f}, PM4 draws dropped {:.1f}, "
      "opaque calls handed to the PM4 {:.1f}; problems so far {}\n",
      last_frame, frame, (draws - last_draws) / f, (ring_draws_dropped - last_dropped) / f, (markers - last_markers) / f,
      bad);
  out += fmt::format(
      "== front end (total): draws {}, flushes {}, shader loads {}, patched loads {}, begin vertices {}, markers {}, "
      "runs {} (unknown {}), recordings {}, recording devices {}, immediate calls on another device {}, publishes {}, "
      "screen extent {}, predicated tiling {}, SetPending_Predicated {}, no scratch {}\n",
      c(g_counts.draws), c(g_counts.flushes), c(g_counts.shader_loads), c(g_counts.patched_loads),
      c(g_counts.begin_vertices), c(g_counts.markers), c(g_counts.runs), c(g_counts.unknown_runs),
      c(g_counts.recordings), c(g_counts.devices), c(g_counts.foreign_immediate), c(g_counts.publishes),
      c(g_counts.screen_extent), c(g_counts.predicated_tiling), c(g_counts.set_pending_predicated),
      c(g_counts.no_scratch));
  last_frame = frame;
  last_draws = draws;
  last_dropped = ring_draws_dropped;
  last_markers = markers;
  std::string path;
  {
    std::lock_guard lock(g_verify_path_mutex);
    path = g_verify.path;
  }
  if (path.size() > 4) {
    path = path.substr(0, path.size() - 4) + "-native.txt";
    if (FILE* file = std::fopen(path.c_str(), "w")) {
      std::fwrite(out.data(), 1, out.size(), file);
      std::fclose(file);
    }
  }
}

void SetFilesDirectory(std::string directory) {
  std::lock_guard lock(g_verify_path_mutex);
  g_verify.path = directory + "/native-d3d-verify.txt";
}

}  // namespace renut::native_d3d
