#pragma once
// Native Direct3D front end: the game's Direct3D draws, state flushes, clears
// and resolves turned into host command lists instead of PM4.
//
// The Xbox 360 Direct3D keeps the whole GPU state in the device (a shadow of
// the registers, with dirty bits) and only writes it out, as PM4, when a draw
// needs it (the SetPending_* functions). The game sets state by writing the
// device directly - much of it inlined into game code by LTCG - so the device
// is the one place the full state lives. Here the draw functions are replaced:
// their state flush is done again from the device, following the PowerPC
// (sub_826567C8, sub_8222BF10, ...), into records of a host list, and the
// draw itself becomes a record. Command buffers get a list of their own,
// which a run appends to the list being recorded. Render targets are the
// bound D3DSurfaces, resolves copy into the D3DTexture they name, vertex and
// index buffers are kept per object. The lists also carry what the GPU writes
// back for the CPU (fences, the swap history, callbacks); nothing walks the
// game's command segments and the ring is not read. The report goes to
// files/native-d3d.txt.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace renut::native_d3d {

// The front end is always on: the lists are the only way anything is drawn.
enum class Mode : uint32_t { kOff = 0, kNative = 2 };
Mode GetMode();

// Record opcodes, in the low 8 bits of a record's first word; the upper 24
// bits hold a count or flags.
enum Op : uint32_t {
  kOpRegs = 1,     // [op | count << 8] first register, count values
  kOpShader = 2,   // [op] type | physical address, dwords, two words unused
  kOpDraw = 3,     // [op | flags << 8] viz token, initiator, dma base, dma size, serial
  kOpBin = 4,      // [op | which << 8] value: 0 mask lo, 1 mask hi, 2 select lo, 3 select hi
  kOpRun = 5,      // [op] index into List::runs
  kOpSwap = 7,     // [op] frontbuffer, width, height: D3DDevice_Swap (presenting is kOpPresent)
  // [op | flags << 8] x1, y1, x2, y2, red, green, blue, alpha, z (float bits), stencil,
  // RB_SURFACE_INFO, RB_COLOR_INFO of targets 0-3, RB_DEPTH_INFO, present (bits 0-3 targets, 4 depth)
  kOpClear = 9,
  // [op] x0, y0, x1, y1 (resolve-aligned), RB_COPY_CONTROL .. RB_COPY_SURFACE_SLICE
  // (0x2318-0x2323), RB_SURFACE_INFO .. RB_COLOR3_INFO (0x2000-0x2005), source slot
  // (0-3 render target, 4 depth), then the destination D3DTexture: its level 0's
  // GPU address (0 when the copy goes to another level, slice or face), width,
  // height, where the source rectangle's corner lands relative to it (dest point
  // minus source corner, x and y) and its pitch in texels
  kOpResolve = 10,
  // [op | slots << 8] for each slot set (bits 0-3 render targets 0-3, 4 depth),
  // in order: the D3DSurface bound there, its +24 (RB_SURFACE_INFO), +28
  // (RB_COLOR_INFO / RB_DEPTH_INFO) and +36 (width - 1 << 18 | height - 1 << 3).
  // Comes with every send of the target registers it stands behind.
  kOpSurfaces = 11,
  // [op] GPU address of the device's writeback block, position, fence: SetFence's
  // two EVENT_WRITE_SHDs (+4 the ring position, +0 the fence), native sync
  kOpFence = 13,
  // [op] frontbuffer GPU address, width, height, then the two swap-history slots the
  // GPU stamps with its vblank counter around the swap (GPU addresses, 0 for none):
  // VdSwap, native sync
  kOpPresent = 14,
  // [op | mask << 8] for each stream in the mask (bits 0-17), its vertex buffer's
  // GPU address and size in bytes (0, 0 for none): the vertex fetch constants
  // 95 - stream sent just before
  kOpStreams = 17,
  // [op] the index buffer's GPU address and size in bytes
  kOpIndices = 18,
  // [op] GPU address, reference, mask: the GPU waits until the dword there,
  // masked, equals the reference (WAIT_REG_MEM; the swap's wait for its
  // vertical blank, which paces the game), native sync
  kOpWaitMemory = 19,
  // [op | count << 8] then count pairs (register, value): what a callback or the swap's pacing
  // does to the GPU's registers, in order, without PM4 words. A register of 0xFFFFFFFF is the
  // interrupt, its value the mask of CPUs. Native sync
  kOpEffects = 20,
};
constexpr uint32_t kResolveWords = 30, kClearWords = 18;

enum DrawFlags : uint32_t {
  kDrawPredicated = 1,    // obeys the bin mask, as every Direct3D draw packet does
  kDrawScreenExtent = 2,  // took Direct3D's screen extent path (not reproduced)
};
// Bits 4-7 of a draw's flags: the engine pass (1-8, 0 for none) the draw was
// made in, from EnginePass().
constexpr uint32_t kDrawPassShift = 4;

// The engine pass (1-8) the calling thread is drawing a model's records in, 0
// outside one: set around sub_823A9908 (guest/game_fixes.cpp), stamped on the
// draws recorded meanwhile. Their pipelines teach Plume each pass's render
// state, which pipeline prediction then builds the passes' records with.
uint8_t& EnginePass();

struct List {
  std::vector<uint32_t> words;
  std::vector<std::shared_ptr<const List>> runs;
  uint32_t draws = 0;
  uint32_t command_buffer = 0;  // guest object, 0 for the immediate list
  // The index buffer last named in this list (kOpIndices).
  uint32_t indices_base = 0, indices_size = 0;
  bool indices_named = false;
  // A command buffer's list from BeginCommandBuffer to EndCommandBuffer: the
  // thread that began it writes it without the front end's lock.
  std::atomic<bool> open{false};
};

// Called by the Direct3D interception layer (d3d_layer.cpp) for every
// intercepted function, before and after the original. OnEnter returns true
// when it handled the call, and the original is skipped.
bool OnEnter(uint32_t function, PPCContext& ctx, uint8_t* base);
void OnLeave(uint32_t function, uint32_t r3, PPCContext& ctx, uint8_t* base);

// The GPU backend's side: the immediate lists published so far, in
// order (each holds what the game recorded up to a ring kick-off).
void TakePublished(std::vector<std::shared_ptr<const List>>& out);

// The executor's counters, written with the front end's to files/native-d3d.txt
// every 300 frames.
void NativeReport(uint64_t frame, uint64_t draws, uint64_t bad,
                  const std::function<std::string()>& executor);
// Where the report goes (the runtime's files directory).
void SetFilesDirectory(std::string directory);
// Native sync: VdSwap's frame (frontbuffer GPU address and size) as a record of
// the swapping device's list; false when the swap goes through the ring.
bool RecordPresent(uint32_t frontbuffer, uint32_t width, uint32_t height);
// Native sync: the GPU backend finished a published list.
void NoteListExecuted();
// Native sync: the GPU backend is running lists or presenting (a wait
// meanwhile is no sign of a missing record).
void SetExecutorBusy(bool busy);
// The doorbell writes nothing to the ring, so a
// publish wakes the GPU backend's worker, which then runs the lists itself.
void SetWake(std::function<void()> wake);
bool WorkPending();
void TakeWorkPending();

}  // namespace renut::native_d3d
