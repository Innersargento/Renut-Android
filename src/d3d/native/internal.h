#pragma once
// Internals of the native Direct3D front end, shared by the files of src/d3d/native/:
//
//   lists.cpp      the recording lists, publishing and scratch guest memory
//   flush.cpp      the state flush, from the PowerPC (SetPending_*)
//   draws.cpp      draws, clears and resolves
//   commands.cpp   command buffers, predication, surface clip, swap
//   sync.cpp       what the GPU writes back for the CPU
//   entry.cpp      the public entry points (native_d3d.h)
//   report.cpp     the report
//
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
#include "d3d/native_d3d.h"
#include "guest/guest_address.h"

#include "d3d/d3d_layer.h"
#include "plume/frame_pacing.h"

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


// Guest functions the front end calls, as the Direct3D it replaces does.
extern "C" {
REX_FUNC(sub_822127F8);  // vertex shader binding check / rebind to the declaration
REX_FUNC(sub_82656DA0);  // D3D_PatchVertexShaderToMatchPixelShader
REX_FUNC(sub_821E5218);  // D3D_PatchVertexShaderToMatchVertexDeclaration
REX_FUNC(sub_82210AB0);  // KickOff: more ring space
void __imp__sub_822052C8(PPCContext& __restrict, uint8_t*);  // InsertCallback, the original
void __imp__sub_8228A748(PPCContext& __restrict, uint8_t*);  // SynchronizeToPresentationInterval
// True while this guest thread is inside Photo Mode's capture routine
// (guest/game_fixes.cpp): the resolve it issues is marked for the CPU read.
bool renut_in_photo_capture();
}

namespace renut::native_d3d {

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
constexpr uint32_t kPredicatedMask = 40, kRingWrite = 48;
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
inline std::atomic<uint32_t> g_draw_serial{0};

// Calls per Direct3D function, for the report.
inline std::array<std::atomic<uint64_t>, renut::d3d::kFunctionCount> g_calls{};
inline thread_local uint32_t t_function = 0;  // the Direct3D function this thread is in

// Calls of a device other than the one that swaps, made while it records
// nothing, go into the immediate list; the report names them. On the console
// they go into that device's own ring, so what they set (the window scissor
// above all: SetSurfaceClip writes it straight into the ring) never reaches
// the swapping device's draws. The renut_native_d3d_foreign_mode cvar: 0 as they come,
// 1 each device's window scissor put back before its calls (default), 2
// dropped.
inline std::array<std::atomic<uint64_t>, renut::d3d::kFunctionCount> g_foreign{};
inline std::atomic<uint32_t> g_foreign_device{0};
inline std::atomic<uint64_t> g_foreign_clip{0}, g_foreign_doorbells{0};
inline std::atomic<int> g_foreign_mode{1};
constexpr const char* kFunctionNames[] = {
#define X(symbol, name) #name,
#include "d3d/d3d_functions.inc"
#undef X
};

// ---------------------------------------------------------------------------
// Counters for the report.

inline struct Counters {
  std::atomic<uint64_t> draws{0}, flushes{0}, shader_loads{0}, patched_loads{0}, literal_blocks{0};
  std::atomic<uint64_t> screen_extent{0}, predicated_tiling{0}, set_pending_predicated{0};
  std::atomic<uint64_t> begin_vertices{0}, markers{0}, runs{0}, recordings{0}, publishes{0};
  std::atomic<uint64_t> no_scratch{0}, hiz_events{0}, unknown_runs{0}, nested_recordings{0};
  std::atomic<uint64_t> surface_clips{0}, tiled_surface_clips{0}, foreign_immediate{0}, devices{0};
  std::atomic<uint64_t> fences{0}, presents{0}, callbacks{0}, doorbells{0}, native_doorbells{0};
  std::atomic<uint64_t> stuck_waits{0}, lists_published{0}, lists_executed{0}, indexed_up{0};
  std::atomic<uint64_t> effect_records{0}, doorbells_skipped{0};
  std::atomic<uint64_t> worker_runs{0};
} g_counts;

// A published list wakes the worker, which runs the lists itself: the ring carries nothing.
inline std::atomic<bool> g_work_pending{false};
inline std::mutex g_wake_mutex;
inline std::shared_ptr<std::function<void()>> g_wake;

// Native sync. The watchdog: a Direct3D wait that has not moved for a while although the
// GPU backend has run every published list means a GPU write the lists do not hold.
inline std::atomic<int32_t> g_executor_busy{0};

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
inline struct FrontEnd {
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
  // the immediate list leaves the GPU backend with (unknown after a
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
inline thread_local uint32_t t_recording_dev = 0;
inline thread_local std::shared_ptr<List> t_recording;

List* OwnRecording(uint32_t dev);

// The front end's lock, unless the call goes into this thread's own command
// buffer.
std::unique_lock<std::recursive_mutex> LockUnlessOwn(uint32_t dev);

// Before a call of `dev` goes into the immediate list: the window scissor it
// set last, when the list leaves the GPU backend with another.
void PutBackClip(uint32_t dev, List& list);

List& Current(uint32_t dev);

void Publish();


// ---------------------------------------------------------------------------
// Scratch guest memory for what Direct3D would put in the ring: vertex data
// of BeginVertices, and vertex shaders patched for this draw's declaration.
// It is read by the GPU backend when it runs the list, so it is reused
// only after it went round: 32 MB, a few dozen frames. Taken by several
// recording threads at once.

inline struct Scratch {
  std::once_flag made;
  uint32_t base = 0, size = 32u << 20;
  std::atomic<uint32_t> offset{0};
} g_scratch;

uint32_t ScratchAlloc(uint32_t bytes);

// A guest dword standing in for SetPending_Shaders' stack slot that
// D3D_PatchVertexShaderToMatchPixelShader updates (SQ_PROGRAM_CNTL); one a
// thread, as the stack slot was.
uint32_t ProgramSlot();

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
void RenderStates(Device& d, uint64_t mask, uint32_t first, uint32_t shadow);

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
bool GuestCommitted(uint32_t address);
#else
constexpr bool GuestCommitted(uint32_t) { return true; }
#endif

void Surfaces(Device& d, uint64_t mask);

// SetPending_AluConstants (sub_8222D0E8): four constants (16 registers) a bit.
void AluConstants(Device& d, uint64_t mask, uint32_t first, uint32_t shadow);

// SetPending_FetchConstants (sub_8222AB88): six registers a slot from +1152.
// (It then invalidates the texture cache, registers 0x5000-0x5002: nothing
// to do on the host.)
void Streams(Device& d, uint32_t slots);
void FetchConstants(Device& d, uint64_t mask);

// The vertex buffers behind the vertex fetch constants just sent: stream s
// is constant 95 - s (fetch slot (95 - s) / 3), set by SetStreamSource from
// the object it keeps at +12460 + 4s (sub_8222A408). Its range is the
// buffer's own (XGSetVertexBufferHeader: +24 the address | 3, +28 the size in
// bytes | endian), which the GPU backend keeps as one buffer.
void Streams(Device& d, uint32_t slots);

// The index buffer an indexed draw reads (SetIndices keeps it at +12436;
// XGSetIndexBufferHeader: +24 the address, +28 the size in bytes), named
// when it changes.
void Indices(Device& d, uint32_t ib);

// SetPending_ClipPlanes (sub_8220E2B0): register 0x2007 again, a coherency
// wait (host: nothing), then four registers a plane from 0x2388.
void ClipPlanes(Device& d, uint64_t mask);

// SetLiteralShaderConstants (sub_8222BDC8): the constants a shader defines in
// its microcode header, loaded from its physical part straight into the
// constant registers (LOAD_ALU_CONSTANT) - never into the device's shadow.
// Entries {u16 constant, u16 dwords, u32 offset} from header + header[20].
void LiteralConstants(Device& d, uint32_t header, uint32_t physical);

// sub_822127F8(device, shader, declaration, binding): whether vertex shader
// binding 0 or 1 matches the declaration (rebinding it in place when the GPU
// is done with it). Called as Direct3D calls it.
bool BindingMatches(Device& d, uint32_t shader, uint32_t declaration, uint32_t binding);

// D3D_IncrementalShaderPatchAndLoad (sub_82656EE0): the vertex shader's
// binding copied where the GPU loads it from (the ring there, scratch here),
// patched for the pixel shader and for the declaration, and loaded.
void PatchAndLoad(Device& d, uint32_t match_ps, uint32_t shader, uint32_t program_cntl, uint32_t declaration,
                  uint32_t vs_info, uint32_t ps_info, uint32_t binding);

// SetPending_HiZEnable (sub_8222A798): works out whether hierarchical Z can
// stay on (device +10560 bit 1) and asks for 0x2203 again. Its cache flush
// event has no host counterpart.
uint64_t PendingHiZEnable(Device& d, uint64_t mask);

// SetPending_Shaders (sub_8222BF10): the shaders' microcode loads, their
// literal constants, the vertex shader's binding to the declaration, and the
// render states that follow from the pair (EDRAM mode, program control).
// Predicated tiling's per-band variants are written once, unpredicated: the
// host draws the scene in one pass.
uint64_t PendingShaders(Device& d, uint64_t r20);

// The flush every draw function begins with, over the given dirty masks
// (all of them for a draw; the inherited ones for a command buffer's run).
// A block's dirty word is cleared once anything of it was sent, as Direct3D
// does.
void Flush(Device& d, const uint64_t (&masks)[5]);

// The render-state part of the flush alone, as D3D_ClearF and Resolve do it
// before their own packets (sub_82223560, sub_8222D6C8): only the bits sent
// are cleared.
void FlushRenderStates(Device& d);

void FlushAll(Device& d);

// The visibility query token every draw packet carries.
uint32_t VizToken(const uint8_t* base, uint32_t dev);

// Large draws are split the way Direct3D splits them (65535 indices a
// packet, continued strips overlapping).
uint32_t SplitCount(const uint8_t* base, uint32_t primitive, uint32_t count);

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
bool DrawIndexedVertices(PPCContext& ctx, uint8_t* base);

// D3DDevice_DrawVertices(device, primitive, start vertex, count)
// D3DDevice_DrawIndexedTessellatedVertices(device, primitive, base vertex,
// start index, count) (sub_82230838): the water. It first sets the
// tessellation registers in the shadow - 0x2284 (low bits 1), 0x2289-0x228F
// from Direct3D's tables, for the adaptive mode (0x2285 & 3 == 2) by patch
// type, otherwise by primitive and index size - and marks them, then flushes
// and draws in the explicit major mode; adaptive draws fetch 32-bit indices,
// as many as the patch's corner count times the count.
constexpr uint32_t kTessSplit = 0x820CF528;
bool DrawIndexedTessellatedVertices(PPCContext& ctx, uint8_t* base);

bool DrawVertices(PPCContext& ctx, uint8_t* base);

// D3DDevice_BeginVertices(device, primitive, vertex count, stride) -> data.
// The vertices go to vertex fetch constant 95 (fetch slot 31), the slot's
// dirty bit is set again so the next draw puts the game's value back, and
// the inlined EndVertices sets the write pointer to +13444.
bool BeginVertices(PPCContext& ctx, uint8_t* base);

// D3DDevice_BeginIndexedVertices(device, primitive, base vertex, vertex count,
// index count, index format, stride, &indices, &vertices) (sub_82656208), in
// native mode: the flush, then both buffers from the scratch instead of the
// ring, the vertices on vertex fetch constant 95 and the indices fetched by
// DMA, and the fields EndIndexedVertices and the next draw use.
bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base);

// D3D_ClearF(device, flags, rect, colour, z, stencil) (sub_82223560), in
// native mode: the clear as a record. Flags: bits 0-3 render targets 0-3,
// 0x10 depth, 0x20 stencil (0x40/0x80 hierarchical stencil: no host
// counterpart); without a depth surface the depth bits go. The rectangle is
// clipped by the viewport and, when on, the scissor, as Direct3D does; an
// empty one clears nothing. Direct3D's render-state flush is kept so the
// lists stay in step, and so are the device bits it leaves (hierarchical Z
// looked at again).
bool ClearNative(PPCContext& ctx, uint8_t* base);

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
uint32_t ResolveScratch();

bool ResolveNative(PPCContext& ctx, uint8_t* base);

// D3DDevice_BeginCommandBuffer(device, command buffer, flags, inherit, ...):
// a list of its own from here to EndCommandBuffer.
void BeginCommandBuffer(PPCContext& ctx);

// D3DDevice_EndCommandBuffer(device): Direct3D flushes what is still pending
// into the buffer (when +108 bit 15 is set) before closing it.
void EndCommandBuffer(PPCContext& ctx, uint8_t* base);

// D3DDevice_RunCommandBuffer(device, command buffer, ...): the pending state
// the buffer inherits goes first (dirty & the buffer's inherit masks at
// +24..+56), then the buffer. Direct3D then marks dirty what the buffer sets.
void RunCommandBuffer(PPCContext& ctx, uint8_t* base);

// D3DDevice_SetPredication(device, mask): SET_BIN_MASK_LO, 0 meaning all.
void SetPredication(PPCContext& ctx);

// D3DDevice_SetCommandBufferPredication(device, lo, hi): SET_BIN_MASK_LO/HI,
// both 0 meaning all.
void SetCommandBufferPredication(PPCContext& ctx);

// SetSurfaceClip (sub_82223270, under SetScissorRect and others): the
// window scissor, written straight into the ring (0x2080-0x2082), never
// kept in the device.
void SetSurfaceClip(PPCContext& ctx, uint8_t* base);

void Swap(PPCContext& ctx);

// ---------------------------------------------------------------------------
// Native sync: what the GPU writes back for the CPU, recorded where Direct3D
// asks for it, so the GPU backend need not walk the game's segments.

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

Effects CallbackEffects(uint32_t flags, uint32_t function, uint32_t context);

void PushEffects(List& list, const Effects& effects);

// D3D::CDevice::SetFence(device, ring) (sub_82278528): two EVENT_WRITE_SHDs
// into the writeback block at +10896, the ring position (+14920 low bits |
// the write pointer) to +4 and the fence (+10908, which then moves on by 2)
// to +0. The original still runs: its packets are never read.
void SetFence(PPCContext& ctx, uint8_t* base);

// D3D::CDevice::AddCallsToPrimaryBuffer(device, calls, count) (sub_82210408):
// the doorbell. Each call is (size, address), an INDIRECT_BUFFER in the primary
// ring; everything recorded so far is published as a list that carries what the
// GPU would have written back, and the worker is woken to run it. The ring is
// not written: the original is skipped for the device that draws and swaps.
bool AddCallsToPrimaryBuffer(PPCContext& ctx, uint8_t*);

// D3D InsertCallback(device, ring, flags, function, context) (sub_822052C8):
// scratch register writes naming the callback, waits for the CPU and the
// interrupt that runs it, as PM4 at `ring`; the list carries their effects.
bool InsertCallback(PPCContext& ctx, uint8_t* base);

// D3D SynchronizeToPresentationInterval(device, immediate) (sub_8228A748):
// the swap paced to the vertical blank. Unless immediate it sets scratch
// register 1 (written back to +10900's block at +4), has the GPU wait for
// that, queues the swap callback (InsertCallback, recorded by its own hook)
// and has the GPU wait until the vertical-blank handler clears it again:
// the GPU backend stalls there, and so does the game behind its
// fences, at the presentation interval it chose (30 FPS).
//
// The frame-rate target (plume/frame_pacing.h) is applied here, per frame, so it
// changes while playing: anything but the game's own 30 FPS asks for immediate
// presentation, without the interval the game stored being touched, and the
// command thread paces the frames instead (plume/gpu/swap.cpp IssueSwap). Back at 30
// the very next swap waits for the vblank again.
bool SynchronizeToPresentationInterval(PPCContext& ctx, uint8_t* base);

// The watchdog, in D3D_CBlocker_Check (sub_8264DCD0), which every Direct3D
// wait calls in its loop: the blocker's device is at +0.
void CheckWait(PPCContext& ctx, uint8_t* base);


}  // renut::native_d3d
