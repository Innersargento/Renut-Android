#pragma once
// Native Direct3D front end: the game's Direct3D draws, state flushes, clears
// and resolves turned into host command lists.
//
// The Xbox 360 Direct3D keeps the whole GPU state in the device (a shadow of
// the registers, with dirty bits) and only sends it when a draw
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

// Record opcodes, in the low 8 bits of a record's first word; the upper 24
// bits hold a count or flags.
enum Op : uint32_t {
  // [op | count << 8] first word, count values: the words of the GPU backend's DrawState (gpu/core/draw_state.h)
  // from the one at `first` on, in the format the device keeps them in
  kOpState = 1,
  // [op | count << 8 | program << kShaderProgramShift] type | physical address, dwords, the shader's
  // name (low, high; 0: none): the hash of the container the shader object was made from, `program`
  // saying which of its programs this is. The microcode at the address is that program as created, and
  // is never written. With no name the GPU backend reads and watches the memory itself. Then count
  // vertex fetches of four words (instruction slot, the instruction's three words) that replace those
  // slots of the microcode in the backend's copy (see BindProgram)
  kOpShader = 2,
  kOpDraw = 3,     // [op | flags << 8] viz token, initiator, dma base, dma size, serial
  kOpRun = 5,      // [op] index into List::runs
  kOpSwap = 7,     // [op] frontbuffer, width, height: D3DDevice_Swap (presenting is kOpPresent)
  // [op | flags << 8] x1, y1, x2, y2, red, green, blue, alpha, z (float bits), stencil,
  // present (bits 0-3 targets, 4 depth): the surfaces are the ones the last kOpSurfaces named
  kOpClear = 9,
  // [op] x0, y0, x1, y1 (resolve-aligned), then the five copy words the host reads, as the device
  // keeps them in the Xenos format: RB_COPY_CONTROL, RB_COPY_DEST_INFO, RB_DEPTH_CLEAR,
  // RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO; the source
  // slot (0-3 render target, 4 depth; bit 8 Photo Mode's capture), then the destination
  // D3DTexture: its level 0's GPU address (0 when the copy goes to a volume's slice), width,
  // height, where the source rectangle's corner lands relative to it (dest point minus source
  // corner, x and y) and its pitch in texels
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
  // masked, equals the reference (the swap's wait for its
  // vertical blank, which paces the game), native sync
  kOpWaitMemory = 19,
  // [op] GPU address, value: the dword there is set (big-endian) and the fence waiters are woken -
  // the swap's pacing marks the writeback block with it. Native sync
  kOpStore = 20,
  // [op] function, context, mask: the GPU backend calls function(context) once per CPU in the
  // mask (bits 0-5), where the list stands: what D3DDevice_InsertCallback asks of the GPU, without
  // the scratch registers and the interrupt handler it goes through on the console
  kOpCallback = 21,
  // [op] the photo texture's base level (GPU address), its row pitch in bytes, the width and the
  // height to copy: Photo Mode's capture (CapturePhoto). The GPU backend reads render target 0 back
  // and writes it into those rows as linear X8R8G8B8 (alpha, red, green, blue in memory order).
  kOpPhoto = 22,
};
constexpr uint32_t kResolveWords = 17, kClearWords = 12, kPhotoWords = 5;
// Bits 17-18 of a kOpShader's first word: the program of the shader object.
constexpr uint32_t kShaderProgramShift = 17;

enum DrawFlags : uint32_t {
  kDrawScreenExtent = 2,  // took Direct3D's screen extent path (not reproduced)
};
// Bits 4-7 of a draw's flags: the engine pass (1-8, 0 for none) the draw was
// made in, from EnginePass().
constexpr uint32_t kDrawPassShift = 4;
// Bits 8-10 of a draw's flags: its EDRAM mode as the device's RB_MODECONTROL holds it (4 colour and
// depth, 5 depth only), so the backend does not read the register.
constexpr uint32_t kDrawModeShift = 8;

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

// Called by the Direct3D interception layer (hooks/layer.cpp) for every
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
// Photo Mode's capture, BOOL sub_82535F20(D3DTexture *photo) (hooked in guest/game_fixes.cpp): the
// frame so far goes into the photo's rows through a kOpPhoto record, and the call returns once the
// GPU backend has written them. The original resolved the screen into a scratch texture and
// untiled it with the CPU, which the host's resolves (kept on the GPU) left black. Sets r3.
void CapturePhoto(PPCContext& ctx, uint8_t* base);
// Native sync: the GPU backend is running lists or presenting (a wait
// meanwhile is no sign of a missing record).
void SetExecutorBusy(bool busy);
// The doorbell writes nothing to the ring, so a
// publish wakes the GPU backend's worker, which then runs the lists itself.
void SetWake(std::function<void()> wake);
bool WorkPending();
// The device that swaps (the title's), 0 before its first act. The calls that name
// no device (D3DDevice_IsFencePending, D3DDevice_BlockOnFence) mean this one.
uint32_t MainDevice();
// True when every published list has run and the GPU backend is idle: a fence wait
// still standing then has nothing left that could end it.
bool ExecutorIdle();
void TakeWorkPending();

// The block the stubbed ring points at, and its size: memory the recompiled code may write
// packets into and nothing reads. 0 when it could not be allocated.
constexpr uint32_t kRingSinkSize = 4u << 20;
uint32_t RingSinkAddress();
// Points a device's ring at the sink, as a kick does (and, for a device recording a command buffer,
// names the segment start EndCommandBuffer sizes the segment from).
void RewindRecordingRing(uint8_t* base, uint32_t device);
// Waits (at most timeout_ms) until no more than max_lists_ahead published lists are waiting for the
// backend: the brake a thread that records command buffers takes before it starts another.
void WaitForBackend(uint32_t max_lists_ahead, uint32_t timeout_ms);

// Fence waits sleep instead of polling. The GPU backend calls NotifyFenceCompleted
// after it writes a fence word; a waiter reads FenceEpoch() before it tests the fence
// and hands that value to FenceWait, which returns once the epoch has moved or after
// timeout_us (a missed wake costs one timeout, never a hang).
uint32_t FenceEpoch();
void FenceWait(uint32_t epoch, uint32_t timeout_us);
void NotifyFenceCompleted();

// The memory of the game's textures, by 4 KB page of the physical range: a count advanced when
// Direct3D frees, makes or unlocks a texture there (d3d/resources/resource.cpp, on the game's
// thread, after it has waited for the GPU to be done with the texture). A resolve into a texture
// takes the count of its first page, and the resolved image is stale once the count has moved:
// the memory is another texture's now. (The GPU backend used to hash the memory under every
// resolved texture, each frame it was sampled, to find that out.)
constexpr uint32_t kTextureEpochPages = 1u << 17;
inline std::atomic<uint32_t> g_texture_epoch[kTextureEpochPages];
inline uint32_t TextureEpochIndex(uint32_t address) {
  return (((address & 0x1FFFFFFF) + (((address >> 20) + 0x200) & 0x1000)) >> 12) & (kTextureEpochPages - 1);
}
inline void BumpTextureMemory(uint32_t address) {
  if (address) g_texture_epoch[TextureEpochIndex(address)].fetch_add(1, std::memory_order_release);
}
inline uint32_t TextureMemoryEpoch(uint32_t address) {
  return g_texture_epoch[TextureEpochIndex(address)].load(std::memory_order_acquire);
}

}  // namespace renut::native_d3d
