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
#include "guest/guest_address.h"

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


REXCVAR_DEFINE_INT32(renut_native_sync_after_swaps, 0, "reNut",
                     "Native sync starts once the game has swapped this many times; before that the "
                     "command processor walks the game's segments. 0 (the default) = from the device's "
                     "first act, so no PM4 is read at all; 3 was the boot walked by the command processor");
REXCVAR_DEFINE_BOOL(renut_native_sync, true, "reNut",
                    "Native Direct3D lists: they also carry the fences, "
                    "swaps and callbacks, and the command processor no longer walks the game's "
                    "command segments");

// Guest functions the front end calls, as the Direct3D it replaces does.
extern "C" {
REX_FUNC(sub_822127F8);  // vertex shader binding check / rebind to the declaration
REX_FUNC(sub_82656DA0);  // D3D_PatchVertexShaderToMatchPixelShader
REX_FUNC(sub_821E5218);  // D3D_PatchVertexShaderToMatchVertexDeclaration
REX_FUNC(sub_82210AB0);  // KickOff: more ring space
void __imp__sub_822052C8(PPCContext& __restrict, uint8_t*);  // InsertCallback, the original
void __imp__sub_8228A748(PPCContext& __restrict, uint8_t*);  // SynchronizeToPresentationInterval
}

namespace renut::native_d3d {
namespace {

using renut::d3d::Function;

// ---------------------------------------------------------------------------
// Guest memory (big-endian; on Android base + address is the mapping).

inline uint8_t U8(const uint8_t* base, uint32_t a) { return *GuestPtr(base, a); }
inline uint16_t U16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, GuestPtr(base, a), 2);
  return __builtin_bswap16(v);
}
inline uint32_t U32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint64_t U64(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, a), 8);
  return __builtin_bswap64(v);
}
inline void S8(uint8_t* base, uint32_t a, uint8_t v) { *GuestPtr(base, a) = v; }
inline void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, a), &v, 4);
}
inline void S64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, a), &v, 8);
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


// Every native draw's serial (the draw record's last word, for traces).
std::atomic<uint32_t> g_draw_serial{0};

// Calls per Direct3D function, for the report.
std::array<std::atomic<uint64_t>, renut::d3d::kFunctionCount> g_calls{};
thread_local uint32_t t_function = 0;  // the Direct3D function this thread is in

// Calls of a device other than the one that swaps, made while it records
// nothing, go into the immediate list; the report names them. On the console
// they go into that device's own ring, so what they set (the window scissor
// above all: SetSurfaceClip writes it straight into the ring) never reaches
// the swapping device's draws. <files>/native-d3d-foreign: 0 as they come,
// 1 each device's window scissor put back before its calls (default), 2
// dropped.
std::array<std::atomic<uint64_t>, renut::d3d::kFunctionCount> g_foreign{};
std::atomic<uint32_t> g_foreign_device{0};
std::atomic<uint64_t> g_foreign_clip{0}, g_foreign_doorbells{0};
std::atomic<int> g_foreign_mode{1};
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
  std::atomic<uint64_t> fences{0}, presents{0}, callbacks{0}, doorbells{0}, native_doorbells{0};
  std::atomic<uint64_t> stuck_waits{0}, lists_published{0}, lists_executed{0}, indexed_up{0};
  std::atomic<uint64_t> effects_checked{0}, effect_mismatches{0}, effect_records{0}, doorbells_skipped{0};
  std::atomic<uint64_t> worker_runs{0};
} g_counts;

// PM4-less sync, live switches (files/renut-pm4-callbacks, files/renut-pm4-doorbell, read every
// 30 frames; no file = the default, 1 and 1): callbacks 0 = the PM4 words InsertCallback wrote go
// into the lists, 1 = the lists carry its effects (kOpEffects), 2 = words executed and the
// effects checked against them. doorbell 0 = AddCallsToPrimaryBuffer writes the ring as ever,
// 1 = it does not, and a publish wakes the worker instead of the ring doing it.
std::atomic<int> g_pm4_callbacks{1};
std::atomic<int> g_pm4_doorbell{1};
std::atomic<bool> g_work_pending{false};
std::mutex g_wake_mutex;
std::shared_ptr<std::function<void()>> g_wake;
std::string g_first_mismatch;

// Native sync. The watchdog: a Direct3D wait that has not moved for a while
// although the command processor has run every published list means a GPU
// write the lists do not hold; it is then completed and the ring walked again.
std::atomic<bool> g_sync_fallback{false};
std::atomic<int32_t> g_executor_busy{0};

// ---------------------------------------------------------------------------
// Lists.

class Builder {
 public:
  explicit Builder(List& list) : list_(list) {}
  void Regs(uint32_t first, const uint32_t* values, uint32_t count) {
    if (!count) return;
    uint32_t* p = Grow(2 + size_t(count));
    p[0] = kOpRegs | (count << 8);
    p[1] = first;
    std::memcpy(p + 2, values, size_t(count) * 4);
  }
  void Reg(uint32_t index, uint32_t value) { Regs(index, &value, 1); }
  // `count` big-endian dwords of guest memory, read now. (The constant blocks
  // are most of a list: one resize, not a push_back a dword.)
  void RegsFromGuest(const uint8_t* base, uint32_t first, uint32_t address, uint32_t count) {
    if (!count) return;
    uint32_t* p = Grow(2 + size_t(count));
    p[0] = kOpRegs | (count << 8);
    p[1] = first;
    const uint8_t* source = GuestPtr(base, address);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      p[2 + i] = __builtin_bswap32(v);
    }
  }
  void Shader(uint32_t type, uint32_t physical, uint32_t dwords, uint64_t hash) {
    uint32_t* p = Grow(5);
    p[0] = kOpShader;
    p[1] = (physical & ~3u) | (type & 3);
    p[2] = dwords;
    p[3] = uint32_t(hash);
    p[4] = uint32_t(hash >> 32);
  }
  size_t Draw(uint32_t flags, uint32_t viz, uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
              uint32_t serial) {
    const size_t at = list_.words.size();
    uint32_t* p = Grow(6);
    p[0] = kOpDraw | (flags << 8);
    p[1] = viz;
    p[2] = initiator;
    p[3] = dma_base;
    p[4] = dma_size;
    p[5] = serial;
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
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height) {
    list_.words.push_back(kOpSwap);
    list_.words.push_back(frontbuffer);
    list_.words.push_back(width);
    list_.words.push_back(height);
  }
  List& list() { return list_; }

 private:
  // `n` more words at the end of the list, for the caller to fill.
  uint32_t* Grow(size_t n) {
    const size_t at = list_.words.size();
    list_.words.resize(at + n);
    return list_.words.data() + at;
  }

  List& list_;
};

// The front end's state. Direct3D is driven from one thread at a time (the
// device's thread ownership); the lock keeps the kick-off publishing honest.
// A command buffer's list is written by the thread that began it without the
// lock (OwnRecording): the render jobs record side by side, and taking it for
// every draw had them queue on each other.
struct FrontEnd {
  std::recursive_mutex mutex;
  std::shared_ptr<List> immediate = std::make_shared<List>();
  // The command buffer each device is recording. The game's render jobs
  // record on devices of their own (the job's context +68), several at once
  // on different threads; the immediate list is the swapping device's.
  std::unordered_map<uint32_t, std::shared_ptr<List>> recording;
  uint32_t main_device = 0;
  std::unordered_map<uint32_t, std::shared_ptr<const List>> command_buffers;
  // Words each command buffer took last time, reserved when it is recorded
  // again (most are recorded every frame).
  std::unordered_map<uint32_t, size_t> command_buffer_words;
  // The window scissor each device set last outside a recording, and the one
  // the immediate list leaves the command processor with (unknown after a
  // command buffer runs: it sets its own).
  std::unordered_map<uint32_t, std::array<uint32_t, 3>> window_clip;
  std::array<uint32_t, 3> immediate_clip{};
  bool immediate_clip_known = false;
  // Where another device's calls go when they are dropped (mode 2).
  List discard;
  std::deque<std::shared_ptr<const List>> published;
  std::mutex published_mutex;
  uint32_t marker_serial = 0;
  // The content recorded since the last doorbell carries its sync records
  // (decided at each doorbell, so a list is all one or the other).
  bool sync_recording = false;
  // Swaps seen: native sync starts once the device is up and has shown a few
  // frames through the ring (its start-up writes scratch registers the CPU
  // waits on, as plain register writes in segments).
  uint32_t swaps_seen = 0;
} g_front;

// The command buffer this thread began on a device, while it is open.
thread_local uint32_t t_recording_dev = 0;
thread_local std::shared_ptr<List> t_recording;

List* OwnRecording(uint32_t dev) {
  if (dev != t_recording_dev || !t_recording || !t_recording->open.load(std::memory_order_acquire)) return nullptr;
  return t_recording.get();
}

// The front end's lock, unless the call goes into this thread's own command
// buffer.
std::unique_lock<std::recursive_mutex> LockUnlessOwn(uint32_t dev) {
  if (OwnRecording(dev)) return {};
  return std::unique_lock<std::recursive_mutex>(g_front.mutex);
}

// Before a call of `dev` goes into the immediate list: the window scissor it
// set last, when the list leaves the command processor with another.
void PutBackClip(uint32_t dev, List& list) {
  auto it = g_front.window_clip.find(dev);
  if (it == g_front.window_clip.end()) return;
  if (g_front.immediate_clip_known && g_front.immediate_clip == it->second) return;
  Builder(list).Regs(0x2080, it->second.data(), 3);
  g_front.immediate_clip = it->second;
  g_front.immediate_clip_known = true;
}

List& Current(uint32_t dev) {
  if (List* own = OwnRecording(dev)) return *own;
  auto it = g_front.recording.find(dev);
  if (it != g_front.recording.end() && it->second) return *it->second;
  const int mode = g_foreign_mode.load(std::memory_order_relaxed);
  if (g_front.main_device && dev != g_front.main_device) {
    g_counts.foreign_immediate.fetch_add(1, std::memory_order_relaxed);
    if (t_function < g_foreign.size()) g_foreign[t_function].fetch_add(1, std::memory_order_relaxed);
    g_foreign_device.store(dev, std::memory_order_relaxed);
    if (mode == 2) {
      if (g_front.discard.words.size() > (1u << 20)) g_front.discard.words.clear();
      return g_front.discard;
    }
  }
  if (mode == 1) PutBackClip(dev, *g_front.immediate);
  return *g_front.immediate;
}

void Publish() {
  if (g_front.immediate->words.empty()) return;
  std::shared_ptr<const List> done = std::move(g_front.immediate);
  g_front.immediate = std::make_shared<List>();
  g_front.immediate->words.reserve(done->words.size());
  {
    std::lock_guard lock(g_front.published_mutex);
    g_front.published.push_back(std::move(done));
  }
  g_counts.publishes.fetch_add(1, std::memory_order_relaxed);
  g_counts.lists_published.fetch_add(1, std::memory_order_relaxed);
  if (g_pm4_doorbell.load(std::memory_order_relaxed) == 1 && g_front.sync_recording) {
    g_work_pending.store(true, std::memory_order_release);
    std::shared_ptr<std::function<void()>> wake;
    {
      std::lock_guard lock(g_wake_mutex);
      wake = g_wake;
    }
    if (wake && *wake) (*wake)();
  }
}

bool SyncWanted();

// ---------------------------------------------------------------------------
// Scratch guest memory for what Direct3D would put in the ring: vertex data
// of BeginVertices, and vertex shaders patched for this draw's declaration.
// It is read by the command processor when it runs the list, so it is reused
// only after it went round: 32 MB, a few dozen frames. Taken by several
// recording threads at once.

struct Scratch {
  std::once_flag made;
  uint32_t base = 0, size = 32u << 20;
  std::atomic<uint32_t> offset{0};
} g_scratch;

uint32_t ScratchAlloc(uint32_t bytes) {
  std::call_once(g_scratch.made, [] {
    auto* memory = rex::system::kernel_memory();
    g_scratch.base =
        memory ? memory->SystemHeapAlloc(g_scratch.size, 4096, rex::memory::kSystemHeapPhysical) : 0;
    if (g_scratch.base) {
      REXLOG_INFO("Renut: native Direct3D scratch {:08X} ({} MB)", g_scratch.base, g_scratch.size >> 20);
    }
  });
  if (!g_scratch.base) {
    g_counts.no_scratch.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  bytes = (bytes + 63) & ~63u;
  if (bytes > g_scratch.size / 4) return 0;
  uint32_t at = g_scratch.offset.load(std::memory_order_relaxed), end;
  do {
    end = at + bytes > g_scratch.size ? bytes : at + bytes;
  } while (!g_scratch.offset.compare_exchange_weak(at, end, std::memory_order_relaxed));
  return g_scratch.base + end - bytes;
}

// A guest dword standing in for SetPending_Shaders' stack slot that
// D3D_PatchVertexShaderToMatchPixelShader updates (SQ_PROGRAM_CNTL); one a
// thread, as the stack slot was.
uint32_t ProgramSlot() {
  thread_local uint32_t slot = 0;
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

// The surfaces behind the target registers just sent. `mask` is the 0x2000
// block's (register 0x2000 + i at bit 63 - i); RB_COLOR_INFO, RB_DEPTH_INFO
// and RB_COLOR1..3_INFO are the bound objects' +28 (SetRenderTarget,
// SetDepthStencilSurface, sub_8227AB68 / sub_8227AF40), which keep the
// objects at +12440..+12452 and +12456. Every SetRenderTarget marks its
// register, so each change of object comes through here.
// Whether `address` lies in committed guest memory. The device keeps object
// pointers in arrays that can hold something else (the stream array has two
// slots past the sixteen streams, and slots the game never set): dereferencing
// those faults where the low guest range is not mapped (Windows), and merely
// reads garbage where it is (Android, where it is left as it was).
#if defined(_WIN32)
bool GuestCommitted(uint32_t address) {
  auto* memory = rex::system::kernel_memory();
  if (!memory) return false;
  const auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  rex::memory::HeapAllocationInfo info{};
  return const_cast<rex::memory::BaseHeap*>(heap)->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit);
}
#else
constexpr bool GuestCommitted(uint32_t) { return true; }
#endif

void Surfaces(Device& d, uint64_t mask) {
  static constexpr uint32_t kSlotOfRegister[6] = {0, 0, 4, 1, 2, 3};
  uint32_t slots = 0;
  for (uint32_t r = 1; r < 6; ++r) {
    if ((mask >> (63 - r)) & 1) slots |= 1u << kSlotOfRegister[r];
  }
  if (!slots) return;
  List& list = d.out.list();
  list.words.push_back(kOpSurfaces | (slots << 8));
  for (uint32_t slot = 0; slot < 5; ++slot) {
    if (!((slots >> slot) & 1)) continue;
    uint32_t surface = U32(d.base, d.dev + (slot < 4 ? 12440 + slot * 4 : 12456));
    if (surface && !GuestCommitted(surface)) surface = 0;
    list.words.push_back(surface);
    for (uint32_t at : {24u, 28u, 36u}) list.words.push_back(surface ? U32(d.base, surface + at) : 0);
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
void Streams(Device& d, uint32_t slots);
void FetchConstants(Device& d, uint64_t mask) {
  uint32_t slot = 0, sent = 0;
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    slot += skip;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, 0x4800 + slot * 6, d.dev + kFetchShadow + slot * 24, run * 6);
    for (uint32_t i = slot; i < slot + run && i < 32; ++i) sent |= 1u << i;
    slot += run;
    mask = run >= 64 ? 0 : mask << run;
  }
  if (sent >> 26) Streams(d, sent);
}

// The vertex buffers behind the vertex fetch constants just sent: stream s
// is constant 95 - s (fetch slot (95 - s) / 3), set by SetStreamSource from
// the object it keeps at +12460 + 4s (sub_8222A408). Its range is the
// buffer's own (XGSetVertexBufferHeader: +24 the address | 3, +28 the size in
// bytes | endian), which the command processor keeps as one buffer.
void Streams(Device& d, uint32_t slots) {
  uint32_t mask = 0;
  for (uint32_t s = 0; s < 18; ++s) {
    if ((slots >> ((95 - s) / 3)) & 1) mask |= 1u << s;
  }
  if (!mask) return;
  List& list = d.out.list();
  list.words.push_back(kOpStreams | (mask << 8));
  for (uint32_t s = 0; s < 18; ++s) {
    if (!((mask >> s) & 1)) continue;
    uint32_t vb = U32(d.base, d.dev + 12460 + s * 4);
    // Streams 16 and 17 ride along with the fetch slot they share with stream
    // 15, and what sits past the device's sixteen sources is not an object.
    if (vb && !GuestCommitted(vb)) vb = 0;
    const uint32_t address = vb ? U32(d.base, vb + 24) & ~3u : 0;
    list.words.push_back(address ? Phys(address) : 0);
    list.words.push_back(address ? U32(d.base, vb + 28) & 0x03FFFFFCu : 0);
  }
}

// The index buffer an indexed draw reads (SetIndices keeps it at +12436;
// XGSetIndexBufferHeader: +24 the address, +28 the size in bytes), named
// when it changes.
void Indices(Device& d, uint32_t ib) {
  List& list = d.out.list();
  if (ib && !GuestCommitted(ib)) ib = 0;
  const uint32_t address = ib ? U32(d.base, ib + 24) : 0;
  const uint32_t base = address ? Phys(address) : 0, size = address ? U32(d.base, ib + 28) : 0;
  if (list.indices_named && list.indices_base == base && list.indices_size == size) return;
  list.indices_named = true;
  list.indices_base = base;
  list.indices_size = size;
  list.words.push_back(kOpIndices);
  list.words.push_back(base);
  list.words.push_back(size);
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
  std::memcpy(GuestPtr(d.base, copy), GuestPtr(d.base, source), bytes);
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
  d.out.Shader(0, Phys(copy), dwords, 0);
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
      d.out.Shader(1, address, dwords, 0);
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
    d.out.Shader(0, address, dwords, 0);
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
    if (d2 & (0xFFFFull << 42)) {
      const uint64_t mask = (d2 << 6) & 0xFFFF000000000000ull;
      RenderStates(d, mask, 0x2000, dev + kRegs2000);
      Surfaces(d, mask);
    }
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
      const uint64_t mask = (d2 << 6) & 0xFFFF000000000000ull;
      RenderStates(d, mask, 0x2000, dev + kRegs2000);
      Surfaces(d, mask);
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

// Runs `fill` on the device's current list, restoring the PPC registers the
// guest calls inside used.
template <typename Fill>
void Capture(PPCContext& ctx, uint8_t* base, uint32_t dev, Fill&& fill) {
  PPCContext saved = ctx;
  Device d{base, dev, ctx, Builder(Current(dev))};
  fill(d);
  ctx = saved;
}

// ---------------------------------------------------------------------------
// Handlers.

// D3DDevice_DrawIndexedVertices(device, primitive, base vertex, start index, count)
bool DrawIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t first = ctx.r6.u32, total = ctx.r7.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  Capture(ctx, base, dev, [&](Device& d) {
    FlushAll(d);
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    Indices(d, ib);
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
  return true;
}

// D3DDevice_DrawVertices(device, primitive, start vertex, count)
// D3DDevice_DrawIndexedTessellatedVertices(device, primitive, base vertex,
// start index, count) (sub_82230838): the water. It first sets the
// tessellation registers in the shadow - 0x2284 (low bits 1), 0x2289-0x228F
// from Direct3D's tables, for the adaptive mode (0x2285 & 3 == 2) by patch
// type, otherwise by primitive and index size - and marks them, then flushes
// and draws in the explicit major mode; adaptive draws fetch 32-bit indices,
// as many as the patch's corner count times the count.
constexpr uint32_t kTessSplit = 0x820CF528;
bool DrawIndexedTessellatedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t first = ctx.r6.u32, total = ctx.r7.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  Capture(ctx, base, dev, [&](Device& d) {
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    S32(base, dev + 10612, (U32(base, dev + 10612) & ~3u) | 1);
    S64(base, dev + 24, U64(base, dev + 24) | (1ull << 50));
    uint32_t corners = 0;
    const bool adaptive = (U32(base, dev + 10616) & 3) == 2;
    if (adaptive) {
      const uint32_t row = 0x82002504 + (primitive - 16) * 12;
      corners = U32(base, row + 8);
      S32(base, dev + 10632, U32(base, row));
      S32(base, dev + 10636, U32(base, row + 4));
      S32(base, dev + 10640, U32(base, row + 8));
      const uint32_t pair = 0x82004140 + (primitive - 16) * 8;
      S32(base, dev + 10644, U32(base, pair));
      S32(base, dev + 10648, U32(base, pair + 4));
      S32(base, dev + 10652, 0x61412107);
      S32(base, dev + 10656, 0x66462606);
    } else {
      const uint32_t row = 0x82183888 + primitive * 12;
      S32(base, dev + 10632, U32(base, row));
      S32(base, dev + 10636, U32(base, row + 4));
      S32(base, dev + 10640, U32(base, row + 8));
      const bool wide = (U32(base, ib) & 0x80000000u) != 0;
      const uint32_t pair = (wide ? 0x82183A08 : 0x82183970) + primitive * 8;
      const uint32_t order = wide ? 0x61412101 : 0x30201000;
      S32(base, dev + 10644, U32(base, pair));
      S32(base, dev + 10648, U32(base, pair + 4));
      S32(base, dev + 10656, order);
      S32(base, dev + 10652, order);
    }
    S64(base, dev + 24, U64(base, dev + 24) | (0x7Full << 39));
    FlushAll(d);
    Indices(d, ib);
    const uint32_t ib_flags = U32(base, ib + 0), ib_base = U32(base, ib + 24);
    const uint32_t endian = ((ib_flags >> 29) & 3) << 30;
    const uint32_t viz = VizToken(base, dev);
    const uint32_t extent = (U8(base, dev + 10940) & 1) ? kDrawScreenExtent : 0;
    uint32_t start = first, count = total;
    while (true) {
      d.out.Reg(0x2102, base_vertex);
      uint32_t chunk = count;
      if (count > 65535) {
        const uint32_t divisor = U32(base, kTessSplit + primitive * 8);
        if (divisor) chunk = ((65535 / divisor) & ~1u) * divisor;
      }
      uint32_t initiator, address, size;
      if (adaptive) {
        const uint32_t indices = chunk * corners;
        initiator = (indices << 16) | (primitive & 0x3F) | 0x900;
        address = Phys(start * 4 + ib_base);
        size = ((indices << 1) & 0x00FFFFFE) | endian;
      } else if (ib_flags & 0x80000000u) {
        initiator = (chunk << 16) | (primitive & 0x3F) | 0x100 | 0x800;
        address = Phys(start * 4 + ib_base);
        size = ((chunk << 1) & 0x00FFFFFE) | endian;
      } else {
        initiator = (chunk << 16) | (primitive & 0x3F) | 0x100;
        address = Phys(start * 2 + ib_base);
        size = (chunk & 0x00FFFFFF) | endian;
      }
      if (extent) g_counts.screen_extent.fetch_add(1, std::memory_order_relaxed);
      d.out.Draw(kDrawPredicated | extent, viz, initiator, address, size, serial);
      g_counts.draws.fetch_add(1, std::memory_order_relaxed);
      const uint32_t left = count - chunk;
      if (!left) break;
      const uint32_t overlap = U32(base, kTessSplit + primitive * 8 + 4);
      start += chunk - overlap;
      count = left + overlap;
    }
  });
  return true;
}

bool DrawVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, first = ctx.r5.u32, total = ctx.r6.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
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
  return true;
}

// D3DDevice_BeginVertices(device, primitive, vertex count, stride) -> data.
// The vertices go to vertex fetch constant 95 (fetch slot 31), the slot's
// dirty bit is set again so the next draw puts the game's value back, and
// the inlined EndVertices sets the write pointer to +13444.
bool BeginVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  g_counts.begin_vertices.fetch_add(1, std::memory_order_relaxed);
  const uint32_t dwords = (count * stride) >> 2;
  const uint32_t data = ScratchAlloc(dwords * 4);
  if (!data) {
    ctx.r3.u64 = 0;
    return true;
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
    d.out.Regs(0x4800 + 31 * 6, fetch, 6);
    d.out.Draw(kDrawPredicated, VizToken(base, dev), (count << 16) | (primitive & 0x3F) | 0x80, 0, 0, serial);
    g_counts.draws.fetch_add(1, std::memory_order_relaxed);
  });
  S64(base, dev + 24, U64(base, dev + 24) | 1);
  S32(base, dev + 13444, U32(base, dev + kRingWrite));
  S32(base, dev + 13448, data);
  S32(base, dev + 13456, dwords);
  ctx.r3.u64 = data;
  return true;
}

// D3DDevice_BeginIndexedVertices(device, primitive, base vertex, vertex count,
// index count, index format, stride, &indices, &vertices) (sub_82656208), in
// native mode: the flush, then both buffers from the scratch instead of the
// ring, the vertices on vertex fetch constant 95 and the indices fetched by
// DMA, and the fields EndIndexedVertices and the next draw use.
bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t vertices = ctx.r6.u32, indices = ctx.r7.u32, format = ctx.r8.u32, stride = ctx.r9.u32;
  const uint32_t index_out = ctx.r10.u32, vertex_out = U32(base, ctx.r1.u32 + 84);
  const bool wide = format & 4;
  const uint32_t vertex_dwords = (vertices * stride) >> 2;
  const uint32_t index_dwords = wide ? indices : (indices + 1) >> 1;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  g_counts.indexed_up.fetch_add(1, std::memory_order_relaxed);
  const uint32_t vertex_data = ScratchAlloc(vertex_dwords * 4), index_data = ScratchAlloc(index_dwords * 4);
  if (!vertex_data || !index_data) {
    if (index_out) S32(base, index_out, 0);
    if (vertex_out) S32(base, vertex_out, 0);
    ctx.r3.u64 = 0x8007000Eu;
    return true;
  }
  Capture(ctx, base, dev, [&](Device& d) {
    const uint8_t old_stride = U8(base, dev + 12528);
    const uint32_t stride_dwords = (stride >> 2) & 0xFF;
    S8(base, dev + 12528, uint8_t(stride_dwords));
    if (stride_dwords != U8(base, dev + 11832) && U32(base, dev + kVertexShader)) {
      S64(base, dev + 16, U64(base, dev + 16) | 0x80000);
    }
    FlushAll(d);
    S8(base, dev + 12528, old_stride);
    d.out.Reg(0x2102, base_vertex);
    const uint32_t fetch[6] = {0, 0, 0, 0, Phys(vertex_data) | 3,
                               0x10000002u | ((vertex_dwords << 2) & 0x03FFFFFCu)};
    d.out.Regs(0x4800 + 31 * 6, fetch, 6);
    const uint32_t endian = (format << 30) & 0xC0000000u;
    const uint32_t initiator = (indices << 16) | (primitive & 0x3F) | (wide ? 0x800 : 0);
    const uint32_t size = (wide ? ((indices << 1) & 0x00FFFFFEu) : (indices & 0x00FFFFFFu)) | endian;
    List& list = d.out.list();
    list.indices_named = false;  // the next indexed draw names its buffer again
    d.out.Draw(kDrawPredicated, VizToken(base, dev), initiator, Phys(index_data), size, serial);
    g_counts.draws.fetch_add(1, std::memory_order_relaxed);
  });
  S64(base, dev + 24, U64(base, dev + 24) | 1);
  if (index_out) S32(base, index_out, index_data);
  if (vertex_out) S32(base, vertex_out, vertex_data);
  S32(base, dev + 13444, U32(base, dev + kRingWrite));
  S32(base, dev + 13448, vertex_data);
  S32(base, dev + 13452, index_data);
  S32(base, dev + 13456, vertex_dwords);
  S32(base, dev + 13460, index_dwords);
  ctx.r3.u64 = 0;
  return true;
}

// D3D_ClearF(device, flags, rect, colour, z, stencil) (sub_82223560), in
// native mode: the clear as a record. Flags: bits 0-3 render targets 0-3,
// 0x10 depth, 0x20 stencil (0x40/0x80 hierarchical stencil: no host
// counterpart); without a depth surface the depth bits go. The rectangle is
// clipped by the viewport and, when on, the scissor, as Direct3D does; an
// empty one clears nothing. Direct3D's render-state flush is kept so the
// lists stay in step, and so are the device bits it leaves (hierarchical Z
// looked at again).
bool ClearNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  const uint32_t rect = ctx.r5.u32, colour = ctx.r6.u32 ? ctx.r6.u32 : 0x82001160u;
  const float z = float(ctx.f1.f64);
  const uint32_t stencil = ctx.r8.u32;
  std::lock_guard lock(g_front.mutex);
  if (!U32(base, dev + 12456)) flags &= ~0xF0u;
  if (!flags) return true;
  auto viewport = [&](uint32_t at) {
    float f;
    const uint32_t bits = U32(base, dev + at);
    std::memcpy(&f, &bits, 4);
    return int32_t(f);
  };
  const int32_t vx = viewport(12648), vy = viewport(12652), vw = viewport(12656), vh = viewport(12660);
  int32_t x1 = std::max(int32_t(U32(base, rect + 0)), vx), y1 = std::max(int32_t(U32(base, rect + 4)), vy);
  int32_t x2 = std::min(int32_t(U32(base, rect + 8)), vx + vw), y2 = std::min(int32_t(U32(base, rect + 12)), vy + vh);
  if (U32(base, dev + 11856)) {
    x1 = std::max(x1, int32_t(U32(base, dev + 12676)));
    y1 = std::max(y1, int32_t(U32(base, dev + 12680)));
    x2 = std::min(x2, int32_t(U32(base, dev + 12684)));
    y2 = std::min(y2, int32_t(U32(base, dev + 12688)));
  }
  if (x2 <= x1 || y2 <= y1) return true;
  Capture(ctx, base, dev, [&](Device& d) {
    FlushRenderStates(d);
    uint32_t present = U32(base, dev + 12456) ? 0x10 : 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (U32(base, dev + 12440 + i * 4)) present |= 1u << i;
    }
    List& list = d.out.list();
    list.words.push_back(kOpClear | (flags << 8));
    for (int32_t v : {x1, y1, x2, y2}) list.words.push_back(uint32_t(v));
    for (uint32_t i = 0; i < 4; ++i) list.words.push_back(U32(base, colour + i * 4));
    uint32_t z_bits;
    std::memcpy(&z_bits, &z, 4);
    list.words.push_back(z_bits);
    list.words.push_back(stencil);
    // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1..3_INFO.
    const uint32_t r = dev + kRegs2000;
    for (uint32_t index : {0u, 1u, 3u, 4u, 5u, 2u}) list.words.push_back(U32(base, r + index * 4));
    list.words.push_back(present);
  });
  S8(base, dev + 10941, U8(base, dev + 10941) | 4);
  S64(base, dev + 16, U64(base, dev + 16) | 0x20000);
  return true;
}

// D3DDevice_Resolve(device, flags, source rect, destination texture,
// destination point, level, slice or face, clear colour, clear z, clear
// stencil, parameters) (sub_8222D6C8), in native mode: the copy registers it
// works out (RB_COPY_CONTROL, _DEST_BASE/_PITCH/_INFO, _SURFACE_SLICE and the
// clear values, through its own helpers) and the rectangle, as a record; the
// copy draw it would write is never made. Transliterated up to where it
// stores those registers; the rest of the device bits it leaves are set
// again here.
extern "C" {
REX_FUNC(sub_82207680);  // D3D_FindSurfaceWithinTexture
REX_FUNC(sub_821FA520);  // D3D_SetTileAndDepthClear
REX_FUNC(sub_82290DE0);  // SetColorClear
}
uint32_t ResolveScratch() {
  static uint32_t block = 0;
  if (!block) {
    if (auto* memory = rex::system::kernel_memory()) block = memory->SystemHeapAlloc(256, 16);
  }
  return block;
}

bool ResolveNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  uint32_t src_rect = ctx.r5.u32;
  const uint32_t texture = ctx.r6.u32;
  uint32_t dest_point = ctx.r7.u32;
  const uint32_t level = ctx.r8.u32, slice = ctx.r9.u32;
  uint32_t clear_color = ctx.r10.u32;
  const double clear_z = ctx.f1.f64;
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t clear_stencil = U32(base, entry_sp + 92), params = U32(base, entry_sp + 100);
  const uint32_t scratch = ResolveScratch();
  if (!scratch || !texture) return false;
  std::lock_guard lock(g_front.mutex);
  PPCContext saved = ctx;
  const uint32_t source = flags & 7;
  if (!(flags & 0x70)) {
    const uint32_t surface = source == 4 ? U32(base, dev + 12456) : U32(base, dev + 12440 + source * 4);
    if (surface) {
      const uint32_t kind = U16(base, surface + 24) & 3;
      flags |= kind == 0 ? 16 : kind == 1 ? 80 : 112;
    }
  }
  if (!clear_color) clear_color = 0x82001160;
  // The destination's fetch constant (header +28..+48).
  const uint32_t d0 = U32(base, texture + 28), d1 = U32(base, texture + 32), d2 = U32(base, texture + 36);
  const uint32_t d3 = U32(base, texture + 40), d5 = U32(base, texture + 48);
  const uint32_t packed = (__builtin_rotateleft32(d3, 2) & 2) + 1;
  const uint32_t dimension = (d5 >> 9) & 3;
  uint32_t width, height, depth_step;
  if (dimension != 2) {
    width = (d2 & 0x1FFF) + packed;
    height = ((d2 >> 13) & 0x1FFF) + packed;
    depth_step = 1;
  } else {
    width = (d2 & 0x7FF) + packed;
    height = ((d2 >> 11) & 0x7FF) + packed;
    depth_step = 8;
  }
  uint32_t address = d1 & 0xFFFFF000u;
  const uint32_t format = d1 & 0x3F;
  uint32_t pitch = __builtin_rotateleft32(d0, 15) & 0x3FE0;
  const uint32_t block_bytes = (U16(base, 0x820052F8 + format * 2) >> 3) & 0x1F;
  // Level or slice: Direct3D's own lookup, outputs in scratch.
  const uint32_t o_pitch = scratch + 0, o_height = scratch + 4, o_width = scratch + 8, o_address = scratch + 12;
  if (level || slice) {
    S32(base, o_height, height);
    S32(base, o_width, width);
    S32(base, o_address, address);
    const uint32_t frame = (entry_sp - 0x400) & ~15u;
    S32(base, frame + 84, o_height);
    S32(base, frame + 92, scratch + 16);
    S32(base, frame + 100, o_address);
    ctx.r1.u64 = frame;
    ctx.r3.u64 = texture;
    ctx.r4.u64 = slice & ~(depth_step - 1);
    ctx.r5.u64 = level;
    ctx.r6.u64 = o_pitch;
    ctx.r7.u64 = scratch + 20;
    ctx.r8.u64 = scratch + 24;
    ctx.r9.u64 = scratch + 28;
    ctx.r10.u64 = o_width;
    sub_82207680(ctx, base);
    ctx = saved;
    height = U32(base, o_height);
    width = U32(base, o_width);
    address = U32(base, o_address);
    if (block_bytes) pitch = U32(base, o_pitch) / block_bytes;
  }
  // The source rectangle (the whole destination level by default) and the
  // destination point.
  int32_t rx1, ry1, rx2, ry2;
  if (src_rect) {
    rx1 = int32_t(U32(base, src_rect)), ry1 = int32_t(U32(base, src_rect + 4));
    rx2 = int32_t(U32(base, src_rect + 8)), ry2 = int32_t(U32(base, src_rect + 12));
  } else {
    rx1 = 0, ry1 = 0, rx2 = int32_t(width), ry2 = int32_t(height);
  }
  if (!dest_point) dest_point = 0x82000EE0;
  const int32_t px = int32_t(U32(base, dest_point)), py = int32_t(U32(base, dest_point + 4));
  const uint32_t physical = address & 0x1FFFFFFF;
  const uint32_t high = ((address >> 20) + 512) & 0x1000;
  uint32_t dest_base = uint32_t((py - ry1) * int32_t(pitch) + ((px - rx1) << 5)) * block_bytes + physical + high;
  const uint32_t dest_height = uint32_t(ry1 - py) + height;
  // RB_COPY_DEST_INFO: format (some remapped), the number type, endian.
  uint32_t dest_format = format;
  switch (format) {
    case 54: dest_format = 7; break;
    case 55: dest_format = 16; break;
    case 56: dest_format = 17; break;
    case 27: dest_format = 30; break;
    case 28: dest_format = 31; break;
    case 29: dest_format = 32; break;
    case 22: case 23: dest_format = 6; break;
    default: break;
  }
  const uint32_t sign = (d0 >> 2) & 3;
  uint32_t number = sign == 1 ? 1 : 0;
  if (d3 & 1) number += 2;
  switch (dest_format) {
    case 30: case 31: case 32: case 36: case 37: case 38: case 57: case 63: number = 7; break;
    default: break;
  }
  const uint32_t not_e = (d3 & 0xE) ? 1 : 0;
  uint32_t bias = ((flags >> 26) & 0x3F);
  bias = ((bias & ~0x100u) | ((not_e << 8) & 0x100)) & 0x13F;
  number = (number & 7) | (bias << 3);
  uint32_t info = (dest_format & 0x3F) | (number << 6);
  info = ((info << 7) & 0xFFFFFF80u) | ((d1 >> 6) & 3);
  const uint32_t dest_pitch = ((dest_height << 16) & 0x3FFF0000u) | (pitch & 0x3FFF);
  if (dimension == 2) {
    S32(base, dev + 10820, pitch * height);
    S64(base, dev + 32, U64(base, dev + 32) | 4);
    info |= (((depth_step - 1) & slice) << 4) & 0x70;
    info |= 8;
  }
  uint32_t samples = 1;
  if (flags & 0x2000) {
    samples = 3;
  } else if (source == 4) {
    samples = 0;
  }
  const uint32_t control = ((samples << 20) & 0x300000) | ((flags & 0x377) - 16);
  S32(base, dev + 10788, info);
  S32(base, dev + 10784, dest_pitch);
  S32(base, dev + 10780, dest_base);
  S32(base, dev + 10776, control);
  S64(base, dev + 32, U64(base, dev + 32) | 0x3C00);
  if (flags & 0x200) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = params;
    ctx.f1.f64 = clear_z;
    ctx.r6.u64 = clear_stencil;
    sub_821FA520(ctx, base);
    ctx = saved;
  }
  if (flags & 0x100) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = params;
    ctx.r5.u64 = control & 7;
    for (uint32_t i = 0; i < 16; ++i) ctx.v1.u8[i] = GuestPtr(base, clear_color)[15 - i];  // as lvx128 loads it
    sub_82290DE0(ctx, base);
    ctx = saved;
  }
  // The rectangle the copy draw would cover, resolve-aligned.
  const int32_t x0 = rx1 & ~7, y0 = ry1 & ~7, x1 = (rx2 + 7) & ~7, y1 = (ry2 + 7) & ~7;
  Capture(ctx, base, dev, [&](Device& d) {
    FlushRenderStates(d);
    List& list = d.out.list();
    list.words.push_back(kOpResolve);
    for (int32_t v : {x0, y0, x1, y1}) list.words.push_back(uint32_t(v));
    // RB_COPY_CONTROL .. RB_COPY_SURFACE_SLICE (0x2318-0x2323) and the
    // surface registers RB_SURFACE_INFO .. RB_COLOR3_INFO (0x2000-0x2005).
    for (uint32_t r = 0x2318; r <= 0x2323; ++r) list.words.push_back(U32(base, dev + kRegs2300 + (r - 0x2300) * 4));
    for (uint32_t r = 0; r < 6; ++r) list.words.push_back(U32(base, dev + kRegs2000 + r * 4));
    list.words.push_back(source);
    // The destination object: the level, slice or face the copy goes to, as
    // Direct3D's own lookup found it (3D slices interleave and stay by address).
    const bool to_object = dimension != 2;
    list.words.push_back(to_object ? Phys(address) : 0);
    list.words.push_back(width);
    list.words.push_back(height);
    list.words.push_back(uint32_t(px - rx1));
    list.words.push_back(uint32_t(py - ry1));
    list.words.push_back(pitch);
  });
  // What the copy leaves in the device: the texture's fence, and the state it
  // would have clobbered marked for the next draw (harmless on the host).
  S32(base, texture + 8, U32(base, dev + 10908));
  uint64_t d2m = U64(base, dev + 16);
  d2m |= (1ull << 41) | (1ull << 40) | (1ull << 39) | 0xE0 | (1ull << 44) | 0x80000 | 8 | 0x18000 | 0x800 | 0x100;
  S64(base, dev + 16, d2m);
  S64(base, dev + 24, U64(base, dev + 24) | (1ull << 35) | 0x80000000ull);
  S64(base, dev + 32, U64(base, dev + 32) | 0x80000);
  ctx = saved;
  return true;
}

// D3DDevice_BeginCommandBuffer(device, command buffer, flags, inherit, ...):
// a list of its own from here to EndCommandBuffer.
void BeginCommandBuffer(PPCContext& ctx) {
  const uint32_t dev = ctx.r3.u32, cb = ctx.r4.u32;
  std::lock_guard lock(g_front.mutex);
  g_counts.recordings.fetch_add(1, std::memory_order_relaxed);
  auto [it, added] = g_front.recording.try_emplace(dev);
  if (added) g_counts.devices.fetch_add(1, std::memory_order_relaxed);
  if (it->second) {
    g_counts.nested_recordings.fetch_add(1, std::memory_order_relaxed);
    it->second->open.store(false, std::memory_order_release);
  }
  auto list = std::make_shared<List>();
  list->command_buffer = cb;
  if (auto words = g_front.command_buffer_words.find(cb); words != g_front.command_buffer_words.end()) {
    list->words.reserve(words->second + words->second / 8 + 64);
  }
  list->open.store(true, std::memory_order_release);
  it->second = list;
  t_recording_dev = dev;
  t_recording = std::move(list);
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
  it->second->open.store(false, std::memory_order_release);
  if (t_recording == it->second) {
    t_recording.reset();
    t_recording_dev = 0;
  }
  g_front.command_buffer_words[it->second->command_buffer] = it->second->words.size();
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
  // A command buffer sets its own window scissor (BeginCommandBuffer does);
  // Direct3D's RunCommandBuffer then sets the device's again.
  g_front.immediate_clip_known = false;
}

// D3DDevice_SetPredication(device, mask): SET_BIN_MASK_LO, 0 meaning all.
void SetPredication(PPCContext& ctx) {
  auto lock = LockUnlessOwn(ctx.r3.u32);
  Builder(Current(ctx.r3.u32)).Bin(0, ctx.r4.u32 ? ctx.r4.u32 : 0xFFFFFFFFu);
}

// D3DDevice_SetCommandBufferPredication(device, lo, hi): SET_BIN_MASK_LO/HI,
// both 0 meaning all.
void SetCommandBufferPredication(PPCContext& ctx) {
  auto lock = LockUnlessOwn(ctx.r3.u32);
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
  auto lock = LockUnlessOwn(dev);
  g_counts.surface_clips.fetch_add(1, std::memory_order_relaxed);
  if (U8(base, dev + 10940) & 0x30) g_counts.tiled_surface_clips.fetch_add(1, std::memory_order_relaxed);
  const uint32_t x1 = ctx.r4.u32, y1 = ctx.r5.u32, x2 = ctx.r6.u32, y2 = ctx.r7.u32;
  const std::array<uint32_t, 3> values = {0, ((y1 << 16) & 0x3FFF0000u) | (x1 & 0x3FFF),
                                          ((y2 << 16) & 0x3FFF0000u) | (x2 & 0x3FFF)};
  if (List* own = OwnRecording(dev)) {
    Builder(*own).Regs(0x2080, values.data(), 3);
    return;
  }
  if (g_front.main_device && dev != g_front.main_device) {
    g_foreign_clip.store((uint64_t(x1 & 0xFFFF) << 48) | (uint64_t(y1 & 0xFFFF) << 32) | ((x2 & 0xFFFF) << 16) |
                             (y2 & 0xFFFF),
                         std::memory_order_relaxed);
  }
  g_front.window_clip[dev] = values;
  List& list = Current(dev);
  if (&list != g_front.immediate.get()) {
    Builder(list).Regs(0x2080, values.data(), 3);
  } else if (g_foreign_mode.load(std::memory_order_relaxed) != 1) {
    // (In mode 1 Current has just put this device's scissor back.)
    Builder(list).Regs(0x2080, values.data(), 3);
    g_front.immediate_clip = values;
    g_front.immediate_clip_known = true;
  }
}

void Swap(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  g_front.main_device = ctx.r3.u32;
  ++g_front.swaps_seen;
  Builder(*g_front.immediate).Swap(ctx.r4.u32, 0, 0);
  Publish();
}

// ---------------------------------------------------------------------------
// Native sync: what the GPU writes back for the CPU, recorded where Direct3D
// asks for it, so the command processor need not walk the game's segments.

bool SyncWanted() {
  return GetMode() == Mode::kNative && REXCVAR_GET(renut_native_sync) &&
         g_front.swaps_seen >= uint32_t(std::max(0, REXCVAR_GET(renut_native_sync_after_swaps))) &&
         !g_sync_fallback.load(std::memory_order_relaxed);
}

// PM4-less callbacks. What InsertCallback (sub_822052C8) writes is a pure function of its
// arguments - it stores packets from registers and changes nothing in the device - so the
// lists can carry its effects instead of its words: the register writes in order and the
// interrupt (the waits it also writes are not needed on the host). Flags: bit 1 the callback
// itself, bit 2 the reset of the callback address (both set when neither is), bit 0 leaves
// out the wait-until write, bits 24-29 the CPUs (4 when none).
using Effects = std::vector<std::pair<uint32_t, uint32_t>>;
constexpr uint32_t kEffectInterrupt = 0xFFFFFFFFu;
constexpr uint32_t kRegCallbackAck = 0x045E, kRegScratch0 = 0x0578, kRegScratch1 = 0x0579,
                   kRegCallbackAddress = 0x057C, kRegCallbackContext = 0x057D, kRegWaitUntil = 0x05C8;
constexpr uint32_t kRegScratchUmsk = 0x01DC, kRegScratchAddr = 0x01DD;

Effects CallbackEffects(uint32_t flags, uint32_t function, uint32_t context) {
  Effects out;
  uint32_t mask = (flags >> 24) & 0x3F;
  if (!mask) mask = 4;
  if (!(flags & 6)) flags |= 6;
  if (flags & 2) {
    if (!(flags & 1)) out.push_back({kRegWaitUntil, 0x20000u});
    out.push_back({kRegCallbackAddress, function});
    out.push_back({kRegCallbackContext, context});
    out.push_back({kRegScratch0, mask});
    out.push_back({kEffectInterrupt, mask});
    out.push_back({kRegCallbackAck, mask});
  }
  if (flags & 4) out.push_back({kRegCallbackAddress, 0x0BADF00Du});
  return out;
}

// The same effects read back from the words the original wrote at ring+4 .. ring+4*words:
// type-0 register writes and the INTERRUPT packet, as the executor's NativeCallback decodes them.
Effects DecodeEffects(uint8_t* base, uint32_t ring, uint32_t words) {
  Effects out;
  for (uint32_t i = 1; i <= words;) {
    const uint32_t header = U32(base, ring + i * 4);
    switch (header >> 30) {
      case 0: {
        const uint32_t first = header & 0x7FFF, n = ((header >> 16) & 0x3FFF) + 1;
        const bool one = (header >> 15) & 1;
        for (uint32_t j = 0; j < n && i + 1 + j <= words; ++j) {
          out.push_back({one ? first : first + j, U32(base, ring + (i + 1 + j) * 4)});
        }
        i += 1 + n;
        break;
      }
      case 1:
        i += 3;
        break;
      case 2:
        i += 1;
        break;
      default: {
        const uint32_t opcode = (header >> 8) & 0x7F, n = ((header >> 16) & 0x3FFF) + 1;
        if (opcode == 0x54 && i + 1 <= words) out.push_back({kEffectInterrupt, U32(base, ring + (i + 1) * 4)});
        i += 1 + n;
        break;
      }
    }
  }
  return out;
}

void PushEffects(List& list, const Effects& effects) {
  list.words.push_back(kOpEffects | (uint32_t(effects.size()) << 8));
  for (const auto& [reg, value] : effects) {
    list.words.push_back(reg);
    list.words.push_back(value);
  }
  g_counts.effect_records.fetch_add(1, std::memory_order_relaxed);
}

std::string EffectsText(const Effects& effects) {
  std::string out;
  for (const auto& [reg, value] : effects) {
    out += reg == kEffectInterrupt ? fmt::format(" int:{:X}", value) : fmt::format(" {:X}={:X}", reg, value);
  }
  return out.empty() ? std::string(" (none)") : out;
}

// D3D::CDevice::SetFence(device, ring) (sub_82278528): two EVENT_WRITE_SHDs
// into the writeback block at +10896, the ring position (+14920 low bits |
// the write pointer) to +4 and the fence (+10908, which then moves on by 2)
// to +0. The original still runs: its packets are never read.
void SetFence(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  std::lock_guard lock(g_front.mutex);
  if (!g_front.sync_recording) return;
  const uint32_t block = U32(base, dev + 10896);
  if (!block) return;
  List& list = Current(dev);
  list.words.push_back(kOpFence);
  list.words.push_back(Phys(block));
  list.words.push_back((U32(base, dev + 14920) & 3) | U32(base, dev + kRingWrite));
  list.words.push_back(U32(base, dev + 10908));
  g_counts.fences.fetch_add(1, std::memory_order_relaxed);
}

// D3D::CDevice::AddCallsToPrimaryBuffer(device, calls, count) (sub_82210408):
// the doorbell. Each call is (size, address) and becomes an INDIRECT_BUFFER in
// the primary ring; everything recorded so far goes with these segments, named
// in the list so the command processor knows which list is which segment.
// Whether the next content carries its sync records is decided here.
bool AddCallsToPrimaryBuffer(PPCContext& ctx, uint8_t* base) {
  if (GetMode() != Mode::kNative) return false;
  const uint32_t calls = ctx.r4.u32, count = ctx.r5.u32;
  std::lock_guard lock(g_front.mutex);
  // A segment recorded with its sync records is one the command processor would not walk
  // either, so for it the IB packet (and the write pointer it moves) need not exist.
  const bool carried = g_front.sync_recording;
  if (g_front.main_device && ctx.r3.u32 != g_front.main_device) g_foreign_doorbells.fetch_add(1, std::memory_order_relaxed);
  List& list = *g_front.immediate;
  list.words.push_back(kOpSegments | (std::min(count, 0xFFFFu) << 8));
  list.words.push_back(g_front.sync_recording ? 1u : 0u);
  for (uint32_t i = 0; i < std::min(count, 0xFFFFu); ++i) {
    list.words.push_back(U32(base, calls + i * 8 + 4) & 0x1FFFFFFFu);
  }
  g_counts.doorbells.fetch_add(1, std::memory_order_relaxed);
  if (g_front.sync_recording) g_counts.native_doorbells.fetch_add(1, std::memory_order_relaxed);
  Publish();
  const bool was = g_front.sync_recording;
  g_front.sync_recording = SyncWanted();
  if (g_front.sync_recording != was) {
    REXLOG_INFO("Renut: native sync {} at doorbell {} (swap {})", g_front.sync_recording ? "on" : "off",
                g_counts.doorbells.load(std::memory_order_relaxed), g_front.swaps_seen);
  }
  const bool skip = carried && g_pm4_doorbell.load(std::memory_order_relaxed) == 1 &&
                    ctx.r3.u32 == g_front.main_device && !g_sync_fallback.load(std::memory_order_relaxed);
  if (skip) g_counts.doorbells_skipped.fetch_add(1, std::memory_order_relaxed);
  return skip;
}

// D3D InsertCallback(device, ring, flags, function, context) (sub_822052C8):
// scratch register writes naming the callback, waits for the CPU and the
// interrupt that runs it, as PM4 at `ring`; the packets it wrote go into the
// list as they are.
bool InsertCallback(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, ring = ctx.r4.u32;
  const uint32_t flags = ctx.r5.u32, function = ctx.r6.u32, context = ctx.r7.u32;
  std::unique_lock lock(g_front.mutex);
  if (!g_front.sync_recording) return false;
  lock.unlock();
  __imp__sub_822052C8(ctx, base);
  const uint32_t end = ctx.r3.u32;
  if (end <= ring || end - ring > 4096) return true;
  lock.lock();
  List& list = Current(dev);
  const uint32_t words = (end - ring) / 4;
  g_counts.callbacks.fetch_add(1, std::memory_order_relaxed);
  const int mode = g_pm4_callbacks.load(std::memory_order_relaxed);
  if (mode >= 1) {
    const Effects want = CallbackEffects(flags, function, context);
    if (mode == 2) {
      const Effects got = DecodeEffects(base, ring, words);
      g_counts.effects_checked.fetch_add(1, std::memory_order_relaxed);
      if (got != want && g_counts.effect_mismatches.fetch_add(1, std::memory_order_relaxed) < 3) {
        g_first_mismatch += fmt::format("   mismatch: flags {:08X} function {:08X} context {:08X}\n      words:{}\n"
                                        "      args :{}\n",
                                        flags, function, context, EffectsText(got), EffectsText(want));
      }
    } else {
      PushEffects(list, want);
      return true;
    }
  }
  list.words.push_back(kOpCallback | (words << 8));
  for (uint32_t i = 1; i <= words; ++i) list.words.push_back(U32(base, ring + i * 4));
  return true;
}

// D3D SynchronizeToPresentationInterval(device, immediate) (sub_8228A748):
// the swap paced to the vertical blank. Unless immediate it sets scratch
// register 1 (written back to +10900's block at +4), has the GPU wait for
// that, queues the swap callback (InsertCallback, recorded by its own hook)
// and has the GPU wait until the vertical-blank handler clears it again:
// the command processor stalls there, and so does the game behind its
// fences, at the presentation interval it chose (30 FPS unless unlocked).
bool SynchronizeToPresentationInterval(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, immediate = ctx.r4.u32;
  std::unique_lock lock(g_front.mutex);
  if (!g_front.sync_recording) return false;
  const uint32_t block = U32(base, dev + 10900);
  const bool wait = !immediate && block;
  if (wait) {
    List& list = Current(dev);
    if (g_pm4_callbacks.load(std::memory_order_relaxed) == 1) {
      PushEffects(list, {{kRegScratch1, 1}});  // the present interval, written back
    } else {
      list.words.push_back(kOpCallback | (2u << 8));
      list.words.push_back(0x00000579u);  // type 0: SCRATCH_REG1
      list.words.push_back(1);
    }
  }
  lock.unlock();
  __imp__sub_8228A748(ctx, base);
  if (wait) {
    lock.lock();
    List& list = Current(dev);
    list.words.push_back(kOpWaitMemory);
    list.words.push_back(Phys(block + 4));
    list.words.push_back(0);
    list.words.push_back(0xFFFFFFFFu);
  }
  return true;
}

// The watchdog, in D3D_CBlocker_Check (sub_8264DCD0), which every Direct3D
// wait calls in its loop: the blocker's device is at +0.
void CheckWait(PPCContext& ctx, uint8_t* base) {
  struct Wait {
    uint32_t blocker = 0, done = 0, calls = 0;
    std::chrono::steady_clock::time_point since;
    bool fired = false;
  };
  thread_local Wait wait;
  if (++wait.calls & 1023) return;
  const uint32_t blocker = ctx.r3.u32, dev = U32(base, blocker);
  const uint32_t block = dev ? U32(base, dev + 10896) : 0;
  if (!block) return;
  const uint32_t done = U32(base, block);
  const auto now = std::chrono::steady_clock::now();
  if (blocker != wait.blocker || done != wait.done) {
    wait = Wait{blocker, done, wait.calls, now, false};
    return;
  }
  if (wait.fired || now - wait.since < std::chrono::milliseconds(1500)) return;
  // Only a wait nothing will end: every published list has run and the
  // command processor is idle.
  if (g_executor_busy.load(std::memory_order_relaxed) > 0 ||
      g_counts.lists_executed.load(std::memory_order_relaxed) <
          g_counts.lists_published.load(std::memory_order_relaxed)) {
    // A wait that stands still while the executor still owes work: not stuck yet.
    REXLOG_DEBUG("Renut: native sync: wait kind {} still after 1.5 s, executor busy {}, lists run {} of {} published, "
                "fence {:X} (issued {:X}), position {:X}",
                U32(base, blocker + 4), g_executor_busy.load(std::memory_order_relaxed),
                g_counts.lists_executed.load(std::memory_order_relaxed),
                g_counts.lists_published.load(std::memory_order_relaxed), done, U32(base, dev + 10908) - 2,
                U32(base, block + 4));
    wait.since = now;
    return;
  }
  wait.fired = true;
  g_counts.stuck_waits.fetch_add(1, std::memory_order_relaxed);
  const uint32_t issued = U32(base, dev + 10908) - 2;
  const uint32_t position = (U32(base, dev + 13244) & 3) | U32(base, dev + 13240);
  REXLOG_WARN("Renut: native sync: a wait (kind {}) stood still 1.5 s with every list run: fence {:X} of {:X}, "
              "position {:X} of {:X}; completing it and walking the ring again",
              U32(base, blocker + 4), done, issued, U32(base, block + 4), position);
  g_sync_fallback.store(true, std::memory_order_relaxed);
  S32(base, block + 4, position);
  S32(base, block, issued);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points.

Mode GetMode() { return Mode::kNative; }

bool OnEnter(uint32_t function, PPCContext& ctx, uint8_t* base) {
  using namespace renut::d3d;
  if (function < g_calls.size() && function != renut::d3d::kD3D_CBlocker_Check) {
    g_calls[function].fetch_add(1, std::memory_order_relaxed);
  }
  t_function = function;
  switch (Function(function)) {
    case kD3DDevice_DrawIndexedVertices:
      return DrawIndexedVertices(ctx, base);
    case kD3DDevice_DrawVertices:
      return DrawVertices(ctx, base);
    case kD3DDevice_BeginVertices:
      return BeginVertices(ctx, base);
    case kD3DDevice_BeginIndexedVertices:
      return BeginIndexedVertices(ctx, base);
    case kD3DDevice_DrawIndexedTessellatedVertices:
      return DrawIndexedTessellatedVertices(ctx, base);
    case kD3D_ClearF:
      return ClearNative(ctx, base);
    case kD3DDevice_Resolve:
      return ResolveNative(ctx, base);
    // Memory export and GPR allocation change nothing the lists draw with
    // (the export address is a coherency base; the dummy draws drain the
    // console's pipeline): their PM4 goes to segments nobody walks.
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
    case kD3D_CDevice_SetFence:
      SetFence(ctx, base);
      return false;
    case kD3D_CDevice_AddCallsToPrimaryBuffer:
      return AddCallsToPrimaryBuffer(ctx, base);
    case kD3D_InsertCallback:
      return InsertCallback(ctx, base);
    case kD3D_SynchronizeToPresentationInterval:
      return SynchronizeToPresentationInterval(ctx, base);
    case kD3D_CBlocker_Check:
      if (REXCVAR_GET(renut_native_sync)) CheckWait(ctx, base);
      return false;
    default:
      return false;
  }
}

// D3DDevice_SetRingBufferParameters(device, ...) (sub_82657508), the device's first act: it
// writes the first segment, whose type-0 packets set up the command processor. The one the CPU
// depends on is the scratch register writeback: SCRATCH_ADDR names the block (device + 10900,
// which SynchronizeToPresentationInterval waits on at +4) and SCRATCH_UMSK (0x20037) which
// registers are written back to it. Every later scratch write the lists carry (callbacks, the
// present interval) lands where this says, so it is a record of its own, ahead of the first
// doorbell, whether or not that segment is walked.
void RingBufferParameters(PPCContext& ctx, uint32_t dev, uint8_t* base) {
  if (ctx.r3.u32 != 0 || !dev) return;  // not S_OK
  const uint32_t block = U32(base, dev + 10900);
  if (!block) return;
  std::lock_guard lock(g_front.mutex);
  PushEffects(Current(dev), {{kRegScratchAddr, Phys(block)}, {kRegScratchUmsk, 0x00020037u}});
  // The only device that sets a ring up is the one that draws and swaps, so from here its
  // doorbells are the main ones; and the lists carry their sync records from the first doorbell
  // when the boot is native (renut_native_sync_after_swaps = 0).
  if (!g_front.main_device) g_front.main_device = dev;
  if (!g_front.sync_recording && SyncWanted()) {
    g_front.sync_recording = true;
    REXLOG_INFO("Renut: native sync on from the device's first act (swap {})", g_front.swaps_seen);
  }
}

void OnLeave(uint32_t function, uint32_t r3, PPCContext& ctx, uint8_t* base) {
  if (function == renut::d3d::kD3DDevice_SetRingBufferParameters) RingBufferParameters(ctx, r3, base);
}

void TakePublished(std::vector<std::shared_ptr<const List>>& out) {
  std::lock_guard lock(g_front.published_mutex);
  while (!g_front.published.empty()) {
    out.push_back(std::move(g_front.published.front()));
    g_front.published.pop_front();
  }
}

// ---------------------------------------------------------------------------
// The report.

namespace {
std::mutex g_report_mutex;
std::string g_report_path;
}  // namespace

void NativeReport(uint64_t frame, uint64_t draws, uint64_t ring_draws_dropped, uint64_t bad,
                  const std::function<std::string()>& executor) {
  static uint64_t last_frame = 0, last_draws = 0, last_dropped = 0;
  if (frame % 30 == 0) {
    // The live switch for other devices' immediate calls.
    std::string path;
    {
      std::lock_guard lock(g_report_mutex);
      path = g_report_path;
    }
    path = path.substr(0, path.rfind('/')) + "/native-d3d-foreign";
    if (FILE* file = std::fopen(path.c_str(), "r")) {
      int mode = 1;
      if (std::fscanf(file, "%d", &mode) == 1 && mode >= 0 && mode <= 2 &&
          mode != g_foreign_mode.exchange(mode, std::memory_order_relaxed)) {
        REXLOG_INFO("Renut: native Direct3D: other devices' immediate calls in mode {}", mode);
      }
      std::fclose(file);
    }
  }
  if (frame % 30 == 0) {
    // The PM4-less switches (see g_pm4_callbacks), read again every 30 frames.
    std::string dir;
    {
      std::lock_guard lock(g_report_mutex);
      dir = g_report_path;
    }
    dir = dir.substr(0, dir.rfind('/') + 1);
    auto read_switch = [&](const char* name, std::atomic<int>& target, int fallback, int top) {
      int value = fallback;
      if (FILE* file = std::fopen((dir + name).c_str(), "r")) {
        int parsed = 0;
        if (std::fscanf(file, "%d", &parsed) == 1) value = std::clamp(parsed, 0, top);
        std::fclose(file);
      }
      if (target.exchange(value, std::memory_order_relaxed) != value) {
        REXLOG_INFO("Renut: {} = {}", name, value);
      }
    };
    if (!dir.empty()) {
      read_switch("renut-pm4-callbacks", g_pm4_callbacks, 1, 2);
      read_switch("renut-pm4-doorbell", g_pm4_doorbell, 1, 1);
    }
  }
  if (frame < last_frame + 300) return;
  const double f = double(frame - last_frame);
  auto c = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  std::string out = fmt::format(
      "native Direct3D lists - frames {}..{}\n\n== per frame: draws {:.1f}, ring draw packets ignored {:.1f}; "
      "problems so far {}\n",
      last_frame, frame, (draws - last_draws) / f, (ring_draws_dropped - last_dropped) / f, bad);
  out += fmt::format(
      "== front end (total): draws {}, flushes {}, shader loads {}, patched loads {}, begin vertices {}, "
      "runs {} (unknown {}), recordings {}, recording devices {}, immediate calls on another device {}, publishes {}, "
      "screen extent {}, predicated tiling {}, SetPending_Predicated {}, no scratch {}\n",
      c(g_counts.draws), c(g_counts.flushes), c(g_counts.shader_loads), c(g_counts.patched_loads),
      c(g_counts.begin_vertices), c(g_counts.runs), c(g_counts.unknown_runs),
      c(g_counts.recordings), c(g_counts.devices), c(g_counts.foreign_immediate), c(g_counts.publishes),
      c(g_counts.screen_extent), c(g_counts.predicated_tiling), c(g_counts.set_pending_predicated),
      c(g_counts.no_scratch));
  out += fmt::format(
      "== sync (total): {} doorbells ({} with native sync), fences {}, presents {}, callbacks {}, lists {} "
      "published / {} run, stuck waits {}{}; BeginIndexedVertices {}\n",
      c(g_counts.doorbells), c(g_counts.native_doorbells), c(g_counts.fences), c(g_counts.presents),
      c(g_counts.callbacks), c(g_counts.lists_published), c(g_counts.lists_executed), c(g_counts.stuck_waits),
      g_sync_fallback.load() ? " (fell back to the ring)" : "", c(g_counts.indexed_up));
  {
    const uint64_t clip = g_foreign_clip.load(std::memory_order_relaxed);
    out += fmt::format(
        "== other devices' immediate calls (mode {}): last device {:08X}, doorbells {}, last surface clip "
        "({},{})-({},{}); per frame by function:",
        g_foreign_mode.load(), g_foreign_device.load(), c(g_foreign_doorbells), clip >> 48, (clip >> 32) & 0xFFFF,
        (clip >> 16) & 0xFFFF, clip & 0xFFFF);
    static std::array<uint64_t, renut::d3d::kFunctionCount> last{};
    for (uint32_t i = 0; i < g_foreign.size(); ++i) {
      const uint64_t n = c(g_foreign[i]);
      if (n != last[i]) out += fmt::format(" {} {:.1f};", kFunctionNames[i], double(n - last[i]) / f);
      last[i] = n;
    }
    out += "\n";
  }
  {
    std::string mismatch;
    {
      std::lock_guard lock(g_front.mutex);
      mismatch = g_first_mismatch;
    }
    out += fmt::format(
        "== pm4less: callbacks mode {} ({} checked against their words, {} mismatches, {} structured records), "
        "doorbell mode {} ({} of {} doorbells without a ring packet), worker runs {}\n{}",
        g_pm4_callbacks.load(), c(g_counts.effects_checked), c(g_counts.effect_mismatches),
        c(g_counts.effect_records), g_pm4_doorbell.load(), c(g_counts.doorbells_skipped), c(g_counts.doorbells),
        c(g_counts.worker_runs), mismatch);
  }
  if (executor) out += executor();
  last_frame = frame;
  last_draws = draws;
  last_dropped = ring_draws_dropped;
  std::string path;
  {
    std::lock_guard lock(g_report_mutex);
    path = g_report_path;
  }
  if (!path.empty()) {
    if (FILE* file = std::fopen(path.c_str(), "w")) {
      std::fwrite(out.data(), 1, out.size(), file);
      std::fclose(file);
    }
  }
}

bool RecordPresent(uint32_t frontbuffer, uint32_t width, uint32_t height) {
  std::lock_guard lock(g_front.mutex);
  if (!g_front.sync_recording || !g_front.main_device) return false;
  uint8_t* base = rex::system::kernel_memory() ? rex::system::kernel_memory()->virtual_membase() : nullptr;
  if (!base) return false;
  const uint32_t dev = g_front.main_device, block = U32(base, dev + 10896);
  if (!block) return false;
  // D3DDevice_Swap stamps two slots of its eight-entry history (+21628 the
  // next, +21624 the oldest the vertical blank has not taken) with the GPU's
  // vblank counter, one just before VdSwap and one after, while fewer than
  // six are outstanding.
  const uint32_t next = U32(base, dev + 21628);
  const bool stamped = next - U32(base, dev + 21624) < 6;
  List& list = *g_front.immediate;
  list.words.push_back(kOpPresent);
  list.words.push_back(frontbuffer);
  list.words.push_back(width);
  list.words.push_back(height);
  list.words.push_back(stamped ? Phys(block + (16 + (next & 7)) * 4) : 0);
  list.words.push_back(stamped ? Phys(block + (16 + ((next + 1) & 7)) * 4) : 0);
  g_counts.presents.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void NoteListExecuted() { g_counts.lists_executed.fetch_add(1, std::memory_order_relaxed); }

void SetExecutorBusy(bool busy) { g_executor_busy.fetch_add(busy ? 1 : -1, std::memory_order_relaxed); }

void SetWake(std::function<void()> wake) {
  std::lock_guard lock(g_wake_mutex);
  g_wake = wake ? std::make_shared<std::function<void()>>(std::move(wake)) : nullptr;
}

bool WorkPending() { return g_work_pending.load(std::memory_order_acquire); }

void TakeWorkPending() {
  g_work_pending.store(false, std::memory_order_release);
  g_counts.worker_runs.fetch_add(1, std::memory_order_relaxed);
}

void SetFilesDirectory(std::string directory) {
  std::lock_guard lock(g_report_mutex);
  g_report_path = directory + "/native-d3d.txt";
}

}  // namespace renut::native_d3d
